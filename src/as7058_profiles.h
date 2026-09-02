// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#ifndef __AS7058_PROFILES_H__
#define __AS7058_PROFILES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "agc_typedefs.h"
#include "as7058_typedefs.h"
#include "bio_spo2_a0_typedefs.h"
#include "error_codes.h"

typedef enum {
    AS7058_APP_PROFILE_ID_LEGACY_DEFAULT = 0,
    AS7058_APP_PROFILE_ID_CLICK_PPG_ECG = 1,
    AS7058_APP_PROFILE_ID_CLICK_SPO2 = 2,
    AS7058_APP_PROFILE_ID_CLICK_GOLDEN = 3,
} as7058_app_profile_id_t;

typedef struct {
    as7058_reg_group_power_t power;
    as7058_reg_group_control_t control;
    as7058_reg_group_led_t led;
    as7058_reg_group_pd_t pd;
    as7058_reg_group_ios_t ios;
    as7058_reg_group_ppg_t ppg;
    as7058_reg_group_ecg_t ecg;
    as7058_reg_group_sinc_t sinc;
    as7058_reg_group_iir_t iir;
    as7058_reg_group_seq_t seq;
    as7058_reg_group_pp_t pp;
    as7058_reg_group_fifo_t fifo;
    uint8_t iir_present;
    uint8_t iir_enabled;
    uint8_t spo2_present;
    uint8_t spo2_enabled;
    as7058_sub_sample_ids_t spo2_red_sub_sample;
    as7058_sub_sample_ids_t spo2_ir_sub_sample;
    as7058_sub_sample_ids_t spo2_ambient_sub_sample;
    bio_spo2_a0_configuration_t spo2_config;
    agc_configuration_t agc_config[AGC_MAX_CHANNEL_CNT];
    uint8_t agc_config_num;
} as7058_sensor_profile_t;

const as7058_sensor_profile_t *
as7058_get_active_profile(void);

err_code_t
as7058_apply_sensor_profile(const as7058_sensor_profile_t *p_profile);

#ifdef __cplusplus
}
#endif

#endif
