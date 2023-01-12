#include "jsd/jsd_epd.h"

#include <assert.h>
#include <string.h>

#include "ethercat.h"
#include "jsd/jsd.h"
#include "jsd/jsd_sdo.h"

#define JSD_EPD_MAX_ERROR_POPS_PER_CYCLE (5)

// Pair of Elmo letter command and corresponding object dictionary index
typedef struct {
  char*    lc_chars;
  uint16_t do_index;
} jsd_epd_lc_pair_t;

// Lookup table to map letter command characters to the corresponding object
// dictionary index. IMPORTANT! This table must be kept in alphabetical order so
// that the lookup function works.
static const jsd_epd_lc_pair_t jsd_epd_lc_lookup_table[] = {
    {"AC", 0x300C},
    {"BP", 0x303D},  // TODO(dloret): verify this is the right
                     // index. Documentation shows multiple
                     // indeces.
    {"CA", 0x3052},
    {"CL", 0x305D},
    {"DC", 0x3078},
    {"ER", 0x30AB},
    {"HL", 0x3111},
    {"LL", 0x31A1},
    {"MC", 0x31BC},
    {"PL", 0x3231},
    {"SF", 0x3297},
    {"UM", 0x32E6},
};

static int jsd_epd_compare_lc_keys(const void* lhs, const void* rhs) {
  const jsd_epd_lc_pair_t* const l = lhs;
  const jsd_epd_lc_pair_t* const r = rhs;

  return strcmp(l->lc_chars, r->lc_chars);
}

/****************************************************
 * Public functions
 ****************************************************/

uint16_t jsd_epd_lc_to_do(char letter_command[2]) {
  jsd_epd_lc_pair_t  key        = {.lc_chars = letter_command};
  jsd_epd_lc_pair_t* found_pair = bsearch(
      &key, jsd_epd_lc_lookup_table,
      sizeof(jsd_epd_lc_lookup_table) / sizeof(jsd_epd_lc_lookup_table[0]),
      sizeof(jsd_epd_lc_lookup_table[0]), jsd_epd_compare_lc_keys);
  return found_pair ? found_pair->do_index : 0x0000;
}

const jsd_epd_state_t* jsd_epd_get_state(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);
  return &self->slave_states[slave_id].epd.pub;
}

void jsd_epd_read(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  // Copy TxPDO data from SOEM's IOmap
  assert(sizeof(jsd_epd_txpdo_data_t) ==
         self->ecx_context.slavelist[slave_id].Ibytes);
  memcpy(&self->slave_states[slave_id].epd.txpdo,
         self->ecx_context.slavelist[slave_id].inputs,
         self->ecx_context.slavelist[slave_id].Ibytes);

  jsd_epd_update_state_from_PDO_data(self, slave_id);
}

void jsd_epd_process(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_process_state_machine(self, slave_id);

  // Copy RxPDO data into SOEM's IOmap
  assert(sizeof(jsd_epd_rxpdo_data_t) ==
         self->ecx_context.slavelist[slave_id].Obytes);
  memcpy(self->ecx_context.slavelist[slave_id].outputs,
         &self->slave_states[slave_id].epd.rxpdo,
         self->ecx_context.slavelist[slave_id].Obytes);
}

void jsd_epd_reset(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  double now = jsd_time_get_mono_time_sec();

  if ((now - self->slave_states[slave_id].epd.last_reset_time) >
      JSD_EPD_RESET_DERATE_SEC) {
    self->slave_states[slave_id].epd.new_reset       = true;
    self->slave_states[slave_id].epd.last_reset_time = now;
  } else {
    WARNING(
        "EPD Reset Derate Protection feature is preventing reset, ignoring "
        "request");
  }
  // TODO(dloret): EGD code prints a warning as an else case about ignoring
  // reset.
}

void jsd_epd_halt(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  self->slave_states[slave_id].epd.new_halt_command = true;
}

void jsd_epd_set_digital_output(jsd_t* self, uint16_t slave_id, uint8_t index,
                                uint8_t output) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);
  assert(index < JSD_EPD_NUM_DIGITAL_OUTPUTS);

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;
  if (output > 0) {
    state->rxpdo.digital_outputs |= (0x01 << (16 + index));
  } else {
    state->rxpdo.digital_outputs &= ~(0x01 << (16 + index));
  }
}

void jsd_epd_set_peak_current(jsd_t* self, uint16_t slave_id,
                              double peak_current) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;

  state->rxpdo.max_current = peak_current * 1e6 / state->motor_rated_current;
}

void jsd_epd_set_motion_command_csp(
    jsd_t* self, uint16_t slave_id,
    jsd_epd_motion_command_csp_t motion_command) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_private_state_t* state     = &self->slave_states[slave_id].epd;
  state->new_motion_command          = true;
  state->requested_mode_of_operation = JSD_EPD_MODE_OF_OPERATION_CSP;
  state->motion_command.csp          = motion_command;
}

/****************************************************
 * Private functions
 ****************************************************/

bool jsd_epd_init(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);
  assert(self->ecx_context.slavelist[slave_id].eep_man == JSD_ELMO_VENDOR_ID);

  ec_slavet* slave = &self->ecx_context.slavelist[slave_id];

  jsd_slave_config_t* config = &self->slave_configs[slave_id];
  config->PO2SO_success      = false;

  // The following disables Complete Access (CA) and was needed in Gold drives
  // to make PDO mapping work.
  // TODO(dloret): Check if disabling CA is really necessary for Platinum
  // drives.
  slave->CoEdetails &= ~ECT_COEDET_SDOCA;

  slave->PO2SOconfigx = jsd_epd_PO2SO_config;

  // Platinum's EtherCAT Slave Controller requires to block LRW.
  slave->blockLRW = 1;

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;
  state->last_reset_time         = 0;

  MSG_DEBUG("TxPDO size: %zu Bytes", sizeof(jsd_epd_txpdo_data_t));
  MSG_DEBUG("RxPDO size: %zu Bytes", sizeof(jsd_epd_rxpdo_data_t));

  state->motor_rated_current = config->epd.continuous_current_limit * 1000;
  if (state->motor_rated_current == 0) {
    ERROR("continuous_current_limit not set on EPD[%d]", slave_id);
    return false;
  }
  jsd_epd_set_peak_current(self, slave_id, config->epd.peak_current_limit);

  state->pub.emcy_error_code = 0;

  return true;
}

int jsd_epd_PO2SO_config(ecx_contextt* ecx_context, uint16_t slave_id) {
  assert(ecx_context);
  assert(ecx_context->slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  // Since this function prototype is forced by SOEM, we have embedded a
  // reference to jsd.slave_configs within the ecx_context and extract it here.
  jsd_slave_config_t* slave_configs =
      (jsd_slave_config_t*)ecx_context->userdata;
  jsd_slave_config_t* config = &slave_configs[slave_id];

  if (!jsd_epd_config_PDO_mapping(ecx_context, slave_id)) {
    ERROR("Failed to map PDO parameters on EPD slave %u", slave_id);
    return 0;
  }

  if (!jsd_epd_config_COE_params(ecx_context, slave_id, config)) {
    ERROR("Failed to set COE parameters on EPD slave %u", slave_id);
    return 0;
  }

  if (!jsd_epd_config_LC_params(ecx_context, slave_id, config)) {
    ERROR("Failed to set LC parameters on EPD slave %u", slave_id);
    return 0;
  }

  config->PO2SO_success = true;
  SUCCESS("EPD[%d] drive parameters successfully configured and verified",
          slave_id);
  return 1;
}

int jsd_epd_config_PDO_mapping(ecx_contextt* ecx_context, uint16_t slave_id) {
  MSG_DEBUG("Attempting to map custom EPD PDOs...");

  //////////////// RxPDO Mapping //////////////////////////
  // TODO(dloret): Not sure if 0x1600-0x1603 are RW in ECAT. EGD used 0x1607 and
  // 0x1608.
  uint16_t map_output_pdos_1602[] = {
      0x0008,          // Number of mapped parameters
      0x0020, 0x607A,  // target_position
      0x0020, 0x60FF,  // target_velocity
      0x0010, 0x6071,  // target_torque
      0x0020, 0x60B0,  // position_offset
      0x0020, 0x60B1,  // velocity_offset
      0x0010, 0x60B2,  // torque_offset
      0x0008, 0x6060,  // mode_of_operation
      0x0010, 0x6073,  // max_current
  };
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1602, 0x00,
                                     sizeof(map_output_pdos_1602),
                                     &map_output_pdos_1602)) {
    return 0;
  }

  // TODO(dloret): Add gain scheduling index mapping
  uint16_t map_output_pdos_1603[] = {
      0x0002,          // Number of mapped parameters
      0x0120, 0x60FE,  // digital_outputs
      0x0010, 0x6040,  // controlword
  };
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1603, 0x00,
                                     sizeof(map_output_pdos_1603),
                                     &map_output_pdos_1603)) {
    return 0;
  }

  // TODO(dloret): not sure if index 0 should be set first to 0, then indexes
  // 1-8, and then index 0 to the number of mapped objects.
  // TODO(dloret): Didn't we disable Complete Access somewhere else?
  uint16_t map_output_RxPDO[] = {0x0002, 0x1602, 0x1603};
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1C12, 0x00,
                                     sizeof(map_output_RxPDO),
                                     &map_output_RxPDO)) {
    return 0;
  }

  //////////////// TxPDO Mapping //////////////////////////
  // TODO(dloret): Not sure if 0x1A00-0x1A03 are RW in ECAT. EGD used 0x1A07 and
  // 0x1A08.
  uint16_t map_input_pdos_1a02[] = {
      0x0008,          // Number of mapped parameters
      0x0020, 0x6064,  // actual_position
      0x0020, 0x6069,  // velocity_actual_value
      0x0010, 0x6078,  // current_actual_value
      0x0008, 0x6061,  // mode_of_operation_display
      0x0020, 0x6079,  // dc_link_circuit_voltage
      0x0020, 0x3610,  // drive_temperature_deg_c
      0x0020, 0x60FD,  // digital_inputs
      0x0110, 0x2205,  // analog_input_1
  };
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1A02, 0x00,
                                     sizeof(map_input_pdos_1a02),
                                     &map_input_pdos_1a02)) {
    return 0;
  }

  uint16_t map_input_pdos_1a03[] = {
      0x0004,          // Number of mapped parameters
      0x0210, 0x2205,  // analog_input_2
      0x0120, 0x3607,  // status_register_1
      0x0220, 0x3607,  // status_register_2
      0x0010, 0x6041,  // statusword
  };
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1A03, 0x00,
                                     sizeof(map_input_pdos_1a03),
                                     &map_input_pdos_1a03)) {
    return 0;
  }

  uint16_t map_input_TxPDO[] = {0x0002, 0x1A02, 0x1A03};
  if (!jsd_sdo_set_ca_param_blocking(ecx_context, slave_id, 0x1C13, 0x00,
                                     sizeof(map_input_TxPDO),
                                     &map_input_TxPDO)) {
    return 0;
  }

  return 1;
}

int jsd_epd_config_COE_params(ecx_contextt* ecx_context, uint16_t slave_id,
                              jsd_slave_config_t* config) {
  // TODO(dloret): original code checks that PROF_POS mode is supported. I might
  // want to switch to JSD_EPD_MODE_OF_OPERATION_PROF_TORQUE as default mode to
  // avoid this.

  // Put drive in PROF_POS mode by default
  int8_t controlword = JSD_EPD_MODE_OF_OPERATION_PROF_POS;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x6060, 0x00,
                                  JSD_SDO_DATA_I8, &controlword)) {
    return 0;
  }

  // Set relative motion to be relative to actual position
  uint16_t pos_opt_code = 0x02;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x60F2, 0x00,
                                  JSD_SDO_DATA_U16, &pos_opt_code)) {
    return 0;
  }

  // Set interpolation time period.
  // Drive actually supports microseconds.
  uint8_t loop_period_ms = config->epd.loop_period_ms;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x60C2, 1,
                                  JSD_SDO_DATA_U8, &loop_period_ms)) {
    return 0;
  }

  // Set Extrapolation Cycles Timeout (5 cycles based on ECAT lib testing)
  // TODO(dloret): confirm whether object 0x2F75 remains unchanged for the
  // Platinum.
  int16_t extra_cycles = 5;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x3675, 0,
                                  JSD_SDO_DATA_I16, &extra_cycles)) {
    return 0;
  }

  // Set Quick Stop option code
  // TODO(dloret): should Quick Stop deceleration (0x6085) be set too?
  int16_t quick_stop_opt_code =
      2;  // Slow down on quick-stop ramp and go to SWITCH ON DISABLED state
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x605A, 0,
                                  JSD_SDO_DATA_I16, &quick_stop_opt_code)) {
    return 0;
  }

  // Set motor rated current equal to the continuous current limit parameter
  uint32_t motor_rated_current = config->epd.continuous_current_limit * 1000.0;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x6075, 0,
                                  JSD_SDO_DATA_U32, &motor_rated_current)) {
    return 0;
  }

  // Set torque slope for profile torque commands
  uint32_t torque_slope = config->epd.torque_slope * 1e6 / motor_rated_current;
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x6087, 0,
                                  JSD_SDO_DATA_U32, &torque_slope)) {
    return 0;
  }

  // Set maximum motor speed
  // First, get feedback counts per electrical cycle (e.g. encoder counts per
  // revolution) because the maximum motor speed parameter expects rpm units.
  int64_t ca_18;
  if (!jsd_sdo_get_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("CA"),
                                  18, JSD_SDO_DATA_I64, &ca_18)) {
    return 0;
  }
  MSG("EPD[%d] read CA[18] = %ld counts per revolution", slave_id, ca_18);
  // Express maximum motor speed in rpm units.
  if (config->epd.max_motor_speed < 0.0) {
    ERROR(
        "EPD[%d] failed to set maximum motor speed (%lf). The parameter must "
        "not be negative.",
        slave_id, config->epd.max_motor_speed);
    return 0;
  }
  uint32_t max_motor_speed_rpm =
      (uint32_t)(config->epd.max_motor_speed / ca_18 * 60.0);
  MSG("EPD[%d] max_motor_speed_rpm = %u", slave_id, max_motor_speed_rpm);
  // Finally, set the maximum motor speed object.
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, 0x6080, 0,
                                  JSD_SDO_DATA_U32, &max_motor_speed_rpm)) {
    return 0;
  }

  return 1;
}

int jsd_epd_config_LC_params(ecx_contextt* ecx_context, uint16_t slave_id,
                             jsd_slave_config_t* config) {
  // TODO(dloret): Verify the types of the corresponding data objects
  // TODO(dloret): double check that attempting to set 0x0000 (i.e. command not
  // found) will result in an error. If that is not the case, then the result of
  // jsd_epd_lc_to_do must be checked before attempting to send the SDO.
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("AC"),
                                  1, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.max_profile_accel)) {
    // TODO(dloret): EGD code warns about a minimum permissible profile
    // acceleration. Not sure if this applies to Platinum.
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("DC"),
                                  1, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.max_profile_decel)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("ER"),
                                  2, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.velocity_tracking_error)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("ER"),
                                  3, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.position_tracking_error)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("PL"),
                                  2, JSD_SDO_DATA_FLOAT,
                                  &config->epd.peak_current_time)) {
    return 0;
  }

  // Note that the maximum current limit is also mapped to the RxPDO.
  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("PL"),
                                  1, JSD_SDO_DATA_FLOAT,
                                  &config->epd.peak_current_limit)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("CL"),
                                  1, JSD_SDO_DATA_FLOAT,
                                  &config->epd.continuous_current_limit)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("CL"),
                                  2, JSD_SDO_DATA_FLOAT,
                                  &config->epd.motor_stuck_current_level_pct)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(
          ecx_context, slave_id, jsd_epd_lc_to_do("CL"), 3, JSD_SDO_DATA_FLOAT,
          &config->epd.motor_stuck_velocity_threshold)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("CL"),
                                  4, JSD_SDO_DATA_FLOAT,
                                  &config->epd.motor_stuck_timeout)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("HL"),
                                  2, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.over_speed_threshold)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("LL"),
                                  3, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.low_position_limit)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("HL"),
                                  3, JSD_SDO_DATA_DOUBLE,
                                  &config->epd.high_position_limit)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("BP"),
                                  1, JSD_SDO_DATA_I16,
                                  &config->epd.brake_engage_msec)) {
    return 0;
  }

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("BP"),
                                  2, JSD_SDO_DATA_I16,
                                  &config->epd.brake_disengage_msec)) {
    return 0;
  }

  // TODO(dloret): Set gain scheduling mode later.

  if (!jsd_sdo_set_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("SF"),
                                  1, JSD_SDO_DATA_I64,
                                  &config->epd.smooth_factor)) {
    return 0;
  }

  // Verify startup parameters
  // TODO(dloret): verify CRC once I know how to retrieve it.

  // Verify current limits
  float drive_max_current = 0;
  if (!jsd_sdo_get_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("MC"),
                                  1, JSD_SDO_DATA_FLOAT, &drive_max_current)) {
    return 0;
  }
  MSG("EPD[%d] Drive Maximum Current is %f A", slave_id, drive_max_current);

  if (config->epd.peak_current_limit > drive_max_current) {
    // TODO(dloret): Check if the drive can even allow to set PL[1] if it is
    // greater than MC[1]. PL[1] is set above.
    ERROR("EPD[%d] Peak Current (%f) cannot exceed Drive Maximum Current (%f)",
          slave_id, config->epd.peak_current_limit, drive_max_current);
    return 0;
  }

  if (config->epd.continuous_current_limit > config->epd.peak_current_limit) {
    // TODO(dloret): this would actually disable CL[1] and is valid. Investigate
    // what is the implication of disabling CL[1].
    ERROR("EPD[%d] Continous Current (%f) should not exceed Peak Current (%f)",
          slave_id, config->epd.continuous_current_limit,
          config->epd.peak_current_limit);
    return 0;
  }

  // Display highest allowed control loop (UM[1]=1 -> current control loop,
  // UM[1]=2 -> velocity control loop, UM[1]=5 -> position control loop).
  int16_t um = 0;
  if (!jsd_sdo_get_param_blocking(ecx_context, slave_id, jsd_epd_lc_to_do("UM"),
                                  1, JSD_SDO_DATA_I16, &um)) {
    return 0;
  }
  MSG("EPD[%d] UM[1] = %d", slave_id, um);

  return 1;
}

void jsd_epd_update_state_from_PDO_data(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;

  state->pub.actual_position = state->txpdo.actual_position;
  state->pub.actual_velocity = state->txpdo.velocity_actual_value;
  state->pub.actual_current  = (double)state->txpdo.current_actual_value *
                              state->motor_rated_current / 1e6;

  state->pub.cmd_position = state->rxpdo.target_position;
  state->pub.cmd_velocity = state->rxpdo.target_velocity;
  state->pub.cmd_current =
      (double)state->rxpdo.target_torque * state->motor_rated_current / 1e6;

  state->pub.cmd_ff_position = state->rxpdo.position_offset;
  state->pub.cmd_ff_velocity = state->rxpdo.velocity_offset;
  state->pub.cmd_ff_current =
      (double)state->rxpdo.torque_offset * state->motor_rated_current / 1e6;
  state->pub.cmd_max_current =
      (double)state->rxpdo.max_current * state->motor_rated_current / 1e6;

  state->pub.actual_mode_of_operation = state->txpdo.mode_of_operation_display;
  // TODO(dloret): EGD code prints a change of mode of operation here.

  // Handle Statusword
  state->pub.actual_state_machine_state =
      state->txpdo.statusword & JSD_EPD_STATE_MACHINE_STATE_BITMASK;
  // TODO(dloret): EGD code prints a change of state here.
  if (state->pub.actual_state_machine_state !=
      state->last_state_machine_state) {
    MSG("EPD[%d] actual State Machine State changed to %s (0x%x)", slave_id,
        jsd_epd_state_machine_state_to_string(
            state->pub.actual_state_machine_state),
        state->pub.actual_state_machine_state);

    if (state->pub.actual_state_machine_state ==
        JSD_EPD_STATE_MACHINE_STATE_FAULT) {
      jsd_sdo_signal_emcy_check(self);
      // TODO(dloret): Check if setting state->new_reset to false like in EGD
      // code is actually needed. Commands are handled after reading functions.
      state->fault_real_time = jsd_time_get_time_sec();
      state->fault_mono_time = jsd_time_get_mono_time_sec();
    }
  }
  state->last_state_machine_state = state->pub.actual_state_machine_state;

  state->pub.warning        = state->txpdo.statusword >> 7 & 0x01;
  state->pub.target_reached = state->txpdo.statusword >> 10 & 0x01;

  // Handle Status Register
  state->pub.servo_enabled = state->txpdo.status_register_1 >> 4 & 0x01;
  state->fault_occured_when_enabled =
      state->txpdo.status_register_1 >> 6 & 0x01;
  // TODO(dloret): Double check this is a proper way to check STO status.
  state->pub.sto_engaged = !((state->txpdo.status_register_1 >> 25 & 0x01) &
                             (state->txpdo.status_register_1 >> 26 & 0x01));
  state->pub.motor_on    = state->txpdo.status_register_1 >> 22 & 0x01;
  state->pub.in_motion   = state->txpdo.status_register_1 >> 23 & 0x01;
  state->pub.hall_state  = state->txpdo.status_register_2 >> 0 & 0x07;

  // TODO(dloret): EGD code prints change in sto_engaged here.

  // Digital inputs
  state->interlock = state->txpdo.digital_inputs >> 3 & 0x01;
  for (int i = 0; i < JSD_EPD_NUM_DIGITAL_INPUTS; ++i) {
    state->pub.digital_inputs[i] =
        state->txpdo.digital_inputs >> (16 + i) & 0x01;
  }

  // Bus voltage
  state->pub.bus_voltage = state->txpdo.dc_link_circuit_voltage / 1000.0;

  // Analog input 1 voltage
  state->pub.analog_input_voltage = state->txpdo.analog_input_1 / 1000.0;

  // Analog input 2 analog to digital conversion
  state->pub.analog_input_adc = state->txpdo.analog_input_2;

  // Drive's temperature
  state->pub.drive_temperature = state->txpdo.drive_temperature_deg_c;
}

void jsd_epd_process_state_machine(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;

  switch (state->pub.actual_state_machine_state) {
    case JSD_EPD_STATE_MACHINE_STATE_NOT_READY_TO_SWITCH_ON:
      // This case should never execute because it is an internal initial state
      // that cannot be monitored by the host.
      break;
    case JSD_EPD_STATE_MACHINE_STATE_SWITCH_ON_DISABLED:
      // Transition to READY TO SWITCH ON
      state->rxpdo.controlword = JSD_EPD_STATE_MACHINE_CONTROLWORD_SHUTDOWN;
      break;
    case JSD_EPD_STATE_MACHINE_STATE_READY_TO_SWITCH_ON:
      // Transition to SWITCHED ON
      state->rxpdo.controlword = JSD_EPD_STATE_MACHINE_CONTROLWORD_SWITCH_ON;
      break;
    case JSD_EPD_STATE_MACHINE_STATE_SWITCHED_ON:
      // Startup, a fault, or the completion of a halt command (i.e. Quick Stop)
      // eventually land in this state. Transition to OPERATION ENABLED if a
      // reset command has been received.
      if (state->new_reset) {
        state->rxpdo.controlword =
            JSD_EPD_STATE_MACHINE_CONTROLWORD_ENABLE_OPERATION;
        state->requested_mode_of_operation = JSD_EPD_MODE_OF_OPERATION_PROF_POS;
        state->rxpdo.mode_of_operation     = state->requested_mode_of_operation;
        state->new_reset                   = false;
      }
      break;
    case JSD_EPD_STATE_MACHINE_STATE_OPERATION_ENABLED:
      // TODO(dloret): Set state->pub.fault_code to JSD_EPD_FAULT_OKAY when
      // available.
      state->pub.emcy_error_code = 0;

      // Handle halt (Quick Stop)
      if (state->new_halt_command) {
        // Make sure OPERATON ENABLED will not be entered immediately after the
        // Quick Stop if a reset command was issued together with the halt.
        state->new_reset = false;
        // Invoke the Quick Stop function
        // TODO(dloret): EGD code overwrites previous controlword, maybe to not
        // change the mode of operation bits. It does not seem to me that is
        // necessary.
        state->rxpdo.controlword = JSD_EPD_STATE_MACHINE_CONTROLWORD_QUICK_STOP;
        state->requested_mode_of_operation = JSD_EPD_MODE_OF_OPERATION_PROF_POS;
        state->rxpdo.mode_of_operation     = state->requested_mode_of_operation;
        break;
      }
      // Set the controlword to a known value before potentially setting its
      // mode of operation bits for profiled position mode. It does not
      // represent a transition.
      state->rxpdo.controlword =
          JSD_EPD_STATE_MACHINE_CONTROLWORD_ENABLE_OPERATION;
      jsd_epd_process_mode_of_operation(self, slave_id);
      break;
    case JSD_EPD_STATE_MACHINE_STATE_QUICK_STOP_ACTIVE:
      // No-op. Since the Quick Stop Option Code (0x605A) is set to 2, the drive
      // transitions into SWITCH ON DISABLED at completion of the Quick Stop.
      // TODO(dloret): If this does not work, try setting the controlword to
      // JSD_EPD_STATE_MACHINE_CONTROLWORD_DISABLE_VOLTAGE which includes QUICK
      // STOP ACTIVE -> SWITCH ON DISABLED.
      break;
    case JSD_EPD_STATE_MACHINE_STATE_FAULT_REACTION_ACTIVE:
      // No-op. Transition from FAULT REACTION ACTIVE to FAULT happens
      // automatically at completion of the fault reaction stop.
      break;
    case JSD_EPD_STATE_MACHINE_STATE_FAULT:;  // Semicolon needed to allow
                                              // placement of declarations after
                                              // label
      jsd_error_cirq_t* error_cirq = &self->slave_errors[slave_id];
      ec_errort         error;
      // Try to recover the EMCY code before transitioning out of FAULT.
      // Hopefully, the corresponding EMCY code has a timestamp greater than
      // when the driver detected the EPD's transition into the FAULT state.
      bool error_found    = false;
      int  num_error_pops = 0;
      // TODO(dloret): Might want to use the non-mutex interface of
      // jsd_error_cirq and incorporate a dedicated mutex for access to the
      // queue here and in the error handling (e.g. pushing errors).
      while (num_error_pops < JSD_EPD_MAX_ERROR_POPS_PER_CYCLE &&
             jsd_error_cirq_pop(error_cirq, &error)) {
        if (ectime_to_sec(error.Time) > state->fault_real_time) {
          // Might want to handle other types of errors too in the future.
          if (error.Etype == EC_ERR_TYPE_EMERGENCY) {
            state->pub.emcy_error_code = error.ErrorCode;
            // TODO(dloret): Set state->pub.fault_code here.

            // TODO(dloret): EGD code prints an error message here with the
            // description of the EMCY code.

            // Transition to SWITCHED ON DISABLED
            state->rxpdo.controlword =
                JSD_EPD_STATE_MACHINE_CONTROLWORD_FAULT_RESET;

            error_found = true;
            break;  // break from the while loop
          }
        }
        ++num_error_pops;
      }
      // If the error has not arrived within 1 second, transition out of FAULT
      // because it might never arrive (e.g. error at startup).
      if (!error_found &&
          jsd_time_get_mono_time_sec() > (1.0 + state->fault_mono_time)) {
        state->pub.emcy_error_code = 0xFFFF;
        // TODO(dloret): Set state->pub.fault_coder here too.

        // Transition to SWITCHED ON DISABLED
        state->rxpdo.controlword =
            JSD_EPD_STATE_MACHINE_CONTROLWORD_FAULT_RESET;
      }
      break;
    default:
      ERROR(
          "EPD[%d] Unknown state machine state: 0x%x. This should never "
          "happen. Exiting.",
          slave_id, state->pub.actual_state_machine_state);
      assert(0);
  }
  state->new_motion_command = false;
  state->new_halt_command   = false;
}

void jsd_epd_process_mode_of_operation(jsd_t* self, uint16_t slave_id) {
  assert(self);
  assert(self->ecx_context.slavelist[slave_id].eep_id == JSD_EPD_PRODUCT_CODE);

  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;

  // TODO(dloret): EGD code prints mode of operation change and warns about
  // changing mode of operation during motion.

  switch (state->requested_mode_of_operation) {
    case JSD_EPD_MODE_OF_OPERATION_DISABLED:
      break;
    case JSD_EPD_MODE_OF_OPERATION_PROF_POS:
      ERROR("JSD_EPD_MODE_OF_OPERATION_PROF_POS not implemented yet.");
      break;
    case JSD_EPD_MODE_OF_OPERATION_PROF_VEL:
      ERROR("JSD_EPD_MODE_OF_OPERATION_PROF_VEL not implemented yet.");
      break;
    case JSD_EPD_MODE_OF_OPERATION_PROF_TORQUE:
      ERROR("JSD_EPD_MODE_OF_OPERATION_PROF_TORQUE not implemented yet.");
      break;
    case JSD_EPD_MODE_OF_OPERATION_CSP:
      jsd_epd_mode_of_op_handle_csp(self, slave_id);
      break;
    case JSD_EPD_MODE_OF_OPERATION_CSV:
      ERROR("JSD_EPD_MODE_OF_OPERATION_CSV not implemented yet.");
      break;
    case JSD_EPD_MODE_OF_OPERATION_CST:
      ERROR("JSD_EPD_MODE_OF_OPERATION_CST not implemented yet.");
      break;
    default:
      ERROR(
          "EPD[%d] Mode of operation: 0x%x not implemented. This should never "
          "happen. Exiting.",
          slave_id, state->requested_mode_of_operation);
      assert(0);
  }
}

void jsd_epd_mode_of_op_handle_csp(jsd_t* self, uint16_t slave_id) {
  jsd_epd_private_state_t* state = &self->slave_states[slave_id].epd;
  jsd_epd_motion_command_t cmd   = state->motion_command;

  state->rxpdo.target_position = cmd.csp.target_position;
  state->rxpdo.position_offset = cmd.csp.position_offset;
  state->rxpdo.target_velocity = 0;
  state->rxpdo.velocity_offset = cmd.csp.velocity_offset;
  state->rxpdo.target_torque   = 0;
  state->rxpdo.torque_offset =
      cmd.csp.torque_offset_amps * 1e6 / state->motor_rated_current;

  state->rxpdo.mode_of_operation = JSD_EPD_MODE_OF_OPERATION_CSP;
}

const char* jsd_epd_state_machine_state_to_string(
    jsd_epd_state_machine_state_t state) {
  switch (state) {
    case JSD_EPD_STATE_MACHINE_STATE_NOT_READY_TO_SWITCH_ON:
      return "Not Ready to Switch On";
    case JSD_EPD_STATE_MACHINE_STATE_SWITCH_ON_DISABLED:
      return "Switch On Disabled";
    case JSD_EPD_STATE_MACHINE_STATE_READY_TO_SWITCH_ON:
      return "Ready to Switch On";
    case JSD_EPD_STATE_MACHINE_STATE_SWITCHED_ON:
      return "Switched On";
    case JSD_EPD_STATE_MACHINE_STATE_OPERATION_ENABLED:
      return "Operation Enabled";
    case JSD_EPD_STATE_MACHINE_STATE_QUICK_STOP_ACTIVE:
      return "Quick Stop Active";
    case JSD_EPD_STATE_MACHINE_STATE_FAULT_REACTION_ACTIVE:
      return "Fault Reaction Active";
    case JSD_EPD_STATE_MACHINE_STATE_FAULT:
      return "Fault";
    default:
      return "Unknown State Machine State";
  }
}
