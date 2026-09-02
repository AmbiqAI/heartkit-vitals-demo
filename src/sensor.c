// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file sensor.c
 * @brief AS7058 PPG+ECG sensor bring-up (NSX port).
 *
 * Adapted from legacy heartkit-vitals-demo src/sensor.c and from the
 * ppg-codec-demo NSX reference port. GPIO/IRQ wiring and transport setup
 * follow the nsx-gpio / nsx-as7058_{i2c,spi} pattern established in
 * ppg-codec-demo. Phase 6 fix: sensor_configure() previously hardcoded the
 * simplified single-wavelength JSON-generated "click_ppg_ecg" profile
 * (ppg1_sub_en=1, one LED only) instead of selecting the real dual-
 * wavelength (Red PPG1_SUB1 + IR PPG1_SUB2) + ECG "click golden" profile
 * via as7058_get_active_profile() (as legacy does) -- this silently drove
 * the wrong/no visible LED and discarded the second wavelength entirely.
 * Fixed here: profile now comes from as7058_get_active_profile()
 * (AS7058_APP_PROFILE, constants.h -- defaults to CLICK_GOLDEN), with the
 * same runtime LED sub1/sub2 override legacy applies, and the callback now
 * extracts both PPG1_SUB1 (Red) and PPG1_SUB2 (IR) into separate
 * ringbuffers plus ECG.
 */
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "agc_typedefs.h"
#include "as7058_chiplib.h"
#include "as7058_extract.h"
#include "as7058_interface.h"
#include "as7058_osal_chiplib.h"
#include "error_codes.h"
#include "nsx_as7058_i2c.h"
#include "nsx_as7058_spi.h"
#include "nsx_core.h"
#include "nsx_gpio.h"
#include "nsx_i2c.h"
#include "nsx_spi.h"

#include "as7058_profiles.h"
#include "constants.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "stimulus.h"

#define SENSOR_RB_LEN (SENSOR_BUF_LEN)

static float32_t s_ppg1_rb_buf[SENSOR_RB_LEN];
static float32_t s_ppg2_rb_buf[SENSOR_RB_LEN];
static float32_t s_ecg_rb_buf[SENSOR_RB_LEN];

rb_config_t rbPpg1Sensor = {
    .buffer = s_ppg1_rb_buf, .dlen = sizeof(float32_t), .size = SENSOR_RB_LEN, .head = 0, .tail = 0,
};
rb_config_t rbPpg2Sensor = {
    .buffer = s_ppg2_rb_buf, .dlen = sizeof(float32_t), .size = SENSOR_RB_LEN, .head = 0, .tail = 0,
};
rb_config_t rbEcgSensor = {
    .buffer = s_ecg_rb_buf, .dlen = sizeof(float32_t), .size = SENSOR_RB_LEN, .head = 0, .tail = 0,
};

/* Extern I2C/SPI configs are provided by the app (board bring-up owns bus
 * ownership); declared here to keep sensor.c self-contained during phase 2. */
extern nsx_i2c_config_t nsxI2cCfg;
extern nsx_spi_config_t nsxSpiCfg;

static volatile as7058_extract_metadata_t g_extract_metadata;
static volatile uint32_t g_as7058_int_isr_count = 0;
static volatile uint32_t g_sensor_irq_notify_missed = 0;
static volatile uint32_t g_ppg_push_count = 0;
static volatile uint32_t g_ppg_drop_count = 0;
static volatile uint32_t g_ecg_push_count = 0;
static volatile uint32_t g_ecg_drop_count = 0;
static TaskHandle_t g_sensor_irq_task_handle = NULL;
static sensor_context_t *g_sensorCtx = NULL;

/* AS7058 INT GPIO ISR-to-ISR interval tracking, exposed via ReportTask's
 * once/sec breadcrumb: confirms whether the sensor's INT line is firing at
 * a uniform, expected cadence (normal FIFO-watermark batching -- e.g. one
 * INT per ~26 ECG samples at 200 Hz is exactly the configured watermark,
 * not a bug) versus genuinely irregular/starved (would show large
 * max-interval outliers relative to the min). Useful ongoing health check
 * after any change touching interrupt priorities, USB/BLE ISR paths, or
 * critical sections that could starve the sensor's edge-triggered INT. */
static volatile uint32_t g_as7058_isr_last_tick = 0;
static volatile uint32_t g_as7058_isr_min_interval_ticks = 0xFFFFFFFFu;
static volatile uint32_t g_as7058_isr_max_interval_ticks = 0;

/* SpO2 calibration coefficients from the active profile, captured at
 * sensor_configure() time -- exposed via sensor_get_spo2_config() so
 * metrics.c's ratiometric SpO2 formula (pk_ppg-based, no AMS on-chip
 * algorithm needed) can use the profile's real a/b/c + dc_comp_red/ir
 * values instead of metrics.c's built-in generic fallback coefficients. */
static bio_spo2_a0_configuration_t g_spo2_config;
static bool g_spo2_config_valid = false;

/* Small LCG PRNG + gaussian approximation for adding synthetic noise to the
 * canned PPG stimulus (ported verbatim from legacy sensor.c) -- keeps the
 * playback waveform from being unnaturally clean. */
static uint32_t g_ppg_stim_prng_state = 0x13579BDFu;

static inline float32_t
ppg_stim_uniform_0_1(void)
{
    g_ppg_stim_prng_state = (1664525u * g_ppg_stim_prng_state) + 1013904223u;
    return (float32_t)(g_ppg_stim_prng_state >> 8) * (1.0f / 16777216.0f);
}

static float32_t
ppg_stim_gaussian_noise(float32_t stddev)
{
    float32_t z = 0.0f;
    for (uint32_t i = 0; i < 12u; i++) {
        z += ppg_stim_uniform_0_1();
    }
    z -= 6.0f;
    return z * stddev;
}

/*
 * Canned patient-data playback (ported from legacy sensor.c
 * load_patient_data). When appState/sensorCtx.inputSource selects a
 * pre-recorded patient (< NUM_INPUT_PTS), the live AS7058 FIFO samples are
 * discarded and the same NUMBER of samples is substituted from the canned
 * stimulus arrays (stimulus.c) -- so playback is paced by the real sensor's
 * sample clock and flows through the identical downstream pipeline.
 * inputSource 0 cycles through all patients back-to-back; 1..NUM_INPUT_PTS-1
 * select a single patient's segment (looped). Stimulus values are already
 * pipeline-scale: they bypass the live-path AGC clip/rescale.
 */
static uint32_t
load_patient_data(uint32_t reqSamples, uint32_t slot)
{
    static size_t _stimulus_slot_idxs[20] = {0};
    uint32_t numSamples = reqSamples;
    uint32_t ptSel = (g_sensorCtx != NULL) ? g_sensorCtx->inputSource : 0;
    float32_t val_f32;
    size_t ptStart = 0;
    size_t ptEnd = 0;
    size_t stimulusIdx = 0;

    for (size_t i = 0; i < numSamples; i++) {
        stimulusIdx = _stimulus_slot_idxs[slot];
        // ECG slot
        if (slot == AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1) {
            ptStart = ptSel > 0 ? PTS_ECG_DATA_LEN * (ptSel - 1) : 0;
            ptEnd = ptSel > 0 ? ptStart + PTS_ECG_DATA_LEN : (NUM_INPUT_PTS - 1) * PTS_ECG_DATA_LEN;
            if (stimulusIdx < ptStart || stimulusIdx >= ptEnd) {
                stimulusIdx = ptStart;
            }
            val_f32 = ecg_stimulus[stimulusIdx];
            ringbuffer_push(&rbEcgSensor, &val_f32, 1);
            g_ecg_push_count++;
        }
        // PPG slots
        else if (slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB1 || slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB2) {
            ptStart = ptSel > 0 ? PTS_PPG_DATA_LEN * (ptSel - 1) : 0;
            ptEnd = ptSel > 0 ? ptStart + PTS_PPG_DATA_LEN : (NUM_INPUT_PTS - 1) * PTS_PPG_DATA_LEN;
            if (stimulusIdx < ptStart || stimulusIdx >= ptEnd) {
                stimulusIdx = ptStart;
            }
            if (slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB1) {
                val_f32 = ppg1_stimulus[stimulusIdx];
                val_f32 += ppg_stim_gaussian_noise(PPG_STIM_GAUSS_STD);
                ringbuffer_push(&rbPpg1Sensor, &val_f32, 1);
            } else {
                val_f32 = ppg2_stimulus[stimulusIdx];
                val_f32 += ppg_stim_gaussian_noise(PPG_STIM_GAUSS_STD);
                ringbuffer_push(&rbPpg2Sensor, &val_f32, 1);
            }
            g_ppg_push_count++;
        }
        stimulusIdx++;
        _stimulus_slot_idxs[slot] = stimulusIdx;
    }
    return numSamples;
}

static inline bool
sensor_live_mode(void)
{
    return (g_sensorCtx == NULL) || (g_sensorCtx->inputSource == LIVE_INPUT_MODE);
}

/*
 * GPIO/IRQ wiring for the AS7058 INT pin. nsx-gpio owns pin config, IRQ
 * bank registration/dispatch, and interrupt clearing (see nsx_gpio_init()),
 * replacing the legacy hand-rolled AmbiqSuite HAL GPIO + NVIC + ISR plumbing.
 */
static err_code_t
as7058_osal_int_pin_read(void *p_ctx, uint8_t *p_state)
{
    (void)p_ctx;
    nsx_gpio_level_t level;
    uint32_t status = nsx_gpio_read(AS7058_OSAL_INT_PIN, &level);
    if (status != NSX_STATUS_SUCCESS) {
        return ERR_SYSTEM_CONFIG;
    }
    *p_state = (uint8_t)level;
    return ERR_SUCCESS;
}

static void
as7058_int_gpio_irq_handler(uint32_t pin, void *ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t nowTick;
    uint32_t lastTick;
    uint32_t deltaTicks;
    (void)pin;
    (void)ctx;

    g_as7058_int_isr_count++;

    nowTick = xTaskGetTickCountFromISR();
    lastTick = g_as7058_isr_last_tick;
    g_as7058_isr_last_tick = nowTick;
    if (g_as7058_int_isr_count > 1u) {
        deltaTicks = nowTick - lastTick;
        if (deltaTicks < g_as7058_isr_min_interval_ticks) {
            g_as7058_isr_min_interval_ticks = deltaTicks;
        }
        if (deltaTicks > g_as7058_isr_max_interval_ticks) {
            g_as7058_isr_max_interval_ticks = deltaTicks;
        }
    }

    sensor_notify_irq_from_isr(&xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static err_code_t
as7058_osal_int_pin_init(void)
{
    nsx_gpio_config_t int_pin_cfg = {
        .api = &nsx_gpio_V0_0_1,
        .pin = AS7058_OSAL_INT_PIN,
        .mode = NSX_GPIO_MODE_INPUT,
        .trigger = NSX_GPIO_TRIGGER_RISING,
        .irq_cb = as7058_int_gpio_irq_handler,
        .irq_ctx = NULL,
    };

    uint32_t status = nsx_gpio_init(&int_pin_cfg);
    if (status != NSX_STATUS_SUCCESS) {
        nsx_printf("AS7058 INT pin config failed: pin=%u status=%lu\n", AS7058_OSAL_INT_PIN,
                   (unsigned long)status);
        return ERR_SYSTEM_CONFIG;
    }

    nsx_printf("AS7058 INT configured: pin=%u trigger=RISING\n", AS7058_OSAL_INT_PIN);
    return ERR_SUCCESS;
}

void
sensor_set_irq_task_handle(TaskHandle_t handle)
{
    taskENTER_CRITICAL();
    g_sensor_irq_task_handle = handle;
    taskEXIT_CRITICAL();
}

void
sensor_notify_irq_from_isr(BaseType_t *p_higher_priority_task_woken)
{
    if (NULL == g_sensor_irq_task_handle) {
        g_sensor_irq_notify_missed++;
        return;
    }
    vTaskNotifyGiveFromISR(g_sensor_irq_task_handle, p_higher_priority_task_woken);
}

void
sensor_process_irq_events(void)
{
    as7058_osal_interrupt_callback();
}

static void
sensor_as7058_callback(err_code_t error,
                        const uint8_t *p_fifo_data,
                        uint16_t fifo_data_size,
                        const agc_status_t *p_agc_statuses,
                        uint8_t agc_statuses_num,
                        as7058_status_events_t sensor_events,
                        const void *p_cb_param)
{
    err_code_t result;
    uint32_t samples[48];
    float samples_f32[48];
    uint16_t sample_cnt;
    size_t pushed;

    M_UNUSED_PARAM(p_agc_statuses);
    M_UNUSED_PARAM(agc_statuses_num);
    M_UNUSED_PARAM(sensor_events);
    M_UNUSED_PARAM(p_cb_param);

    if (error != ERR_SUCCESS || p_fifo_data == NULL || fifo_data_size == 0) {
        return;
    }

    // PPG1_SUB1 (Red PPG channel)
    sample_cnt = (uint16_t)(sizeof(samples) / sizeof(samples[0]));
    result = as7058_extract_samples(AS7058_SUB_SAMPLE_ID_PPG1_SUB1, p_fifo_data, fifo_data_size, samples,
                                     &sample_cnt, (as7058_extract_metadata_t *)&g_extract_metadata);
    if (result == ERR_SUCCESS && sample_cnt > 0) {
        if (!sensor_live_mode()) {
            /* Canned patient mode: substitute the same number of samples from
             * the stimulus arrays (paced by the live sensor's sample clock). */
            load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_PPG1_SUB1);
        } else {
            for (uint16_t i = 0; i < sample_cnt; i++) {
                /* Raw AS7058 PPG counts run ~10^5-10^6 (PPG_AGC_MIN..PPG_AGC_MAX,
                 * constants.h). Clip to the AGC operating range and rescale down
                 * to a small int16-friendly span -- matches legacy sensor.c's
                 * per-sample CLIP/-=/ /=16 exactly. Without this, raw counts
                 * blow past int16 range downstream (TX packing, DSP windows),
                 * which is what made the live PPG waveform look flat/dead
                 * except for large step artifacts on full cover/uncover. */
                float32_t val = (float32_t)samples[i];
                val = CLIP(val, PPG_AGC_MIN, PPG_AGC_MAX);
                val -= PPG_AGC_MIN;
                val /= 16.0f;
                samples_f32[i] = val;
            }
            pushed = ringbuffer_push(&rbPpg1Sensor, samples_f32, sample_cnt);
            g_ppg_push_count += (uint32_t)pushed;
            if (pushed < sample_cnt) {
                g_ppg_drop_count += (uint32_t)(sample_cnt - pushed);
            }
        }
    }

    // PPG1_SUB2 (IR PPG channel) -- only present when the active profile
    // enables a 2nd sub-sample (ppg1_sub_en bit 1); see sensor_configure().
    sample_cnt = (uint16_t)(sizeof(samples) / sizeof(samples[0]));
    result = as7058_extract_samples(AS7058_SUB_SAMPLE_ID_PPG1_SUB2, p_fifo_data, fifo_data_size, samples,
                                     &sample_cnt, (as7058_extract_metadata_t *)&g_extract_metadata);
    if (result == ERR_SUCCESS && sample_cnt > 0) {
        if (!sensor_live_mode()) {
            load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_PPG1_SUB2);
        } else {
            for (uint16_t i = 0; i < sample_cnt; i++) {
                float32_t val = (float32_t)samples[i];
                val = CLIP(val, PPG_AGC_MIN, PPG_AGC_MAX);
                val -= PPG_AGC_MIN;
                val /= 16.0f;
                samples_f32[i] = val;
            }
            pushed = ringbuffer_push(&rbPpg2Sensor, samples_f32, sample_cnt);
            g_ppg_push_count += (uint32_t)pushed;
            if (pushed < sample_cnt) {
                g_ppg_drop_count += (uint32_t)(sample_cnt - pushed);
            }
        }
    }

    // ECG_SEQ1_SUB1 (ECG channel)
    sample_cnt = (uint16_t)(sizeof(samples) / sizeof(samples[0]));
    result = as7058_extract_samples(AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1, p_fifo_data, fifo_data_size, samples,
                                     &sample_cnt, (as7058_extract_metadata_t *)&g_extract_metadata);
    if (result == ERR_SUCCESS && sample_cnt > 0) {
        if (!sensor_live_mode()) {
            load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1);
        } else {
            for (uint16_t i = 0; i < sample_cnt; i++) {
                samples_f32[i] = (float)samples[i];
            }
            pushed = ringbuffer_push(&rbEcgSensor, samples_f32, sample_cnt);
            g_ecg_push_count += (uint32_t)pushed;
            if (pushed < sample_cnt) {
                g_ecg_drop_count += (uint32_t)(sample_cnt - pushed);
            }
        }
    }
}

err_code_t
sensor_init(sensor_context_t *ctx)
{
    err_code_t result = ERR_SUCCESS;

    g_sensorCtx = ctx;

    result = as7058_osal_int_pin_init();
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_osal_int_pin_init returned error code %d.\n", result);
        return result;
    }

#if AS7058_USE_SPI
    static nsx_as7058_spi_transport_t s_as7058_transport = {0};
    s_as7058_transport.p_spi_cfg = &nsxSpiCfg;
    s_as7058_transport.cs_channel = 0;
    s_as7058_transport.p_pin_ctx = NULL;
    s_as7058_transport.read_pin_state = as7058_osal_int_pin_read;
    result = nsx_as7058_spi_configure_osal(&s_as7058_transport);
#else
    static nsx_as7058_i2c_transport_t s_as7058_transport = {0};
    s_as7058_transport.p_i2c_cfg = &nsxI2cCfg;
    s_as7058_transport.i2c_addr = AS7058_I2C_ADDR;
    s_as7058_transport.p_pin_ctx = NULL;
    s_as7058_transport.read_pin_state = as7058_osal_int_pin_read;
    result = nsx_as7058_i2c_configure_osal(&s_as7058_transport);
#endif
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058 transport configure returned error code %d.\n", result);
        return result;
    }

    result = as7058_initialize(sensor_as7058_callback, NULL, NULL, NULL);
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_initialize returned error code %d.\n", result);
        return result;
    }

    nsx_printf("AS7058 sensor_init OK (transport=%s addr/cs=%s)\n", AS7058_USE_SPI ? "SPI" : "I2C",
               AS7058_USE_SPI ? "cs0" : "0x55");

    return result;
}

err_code_t
sensor_configure(void)
{
    err_code_t result;
    const as7058_sensor_profile_t *p_active_profile;
    as7058_sensor_profile_t profile;
    bool spo2_profile_enabled;

    p_active_profile = as7058_get_active_profile();
    if (NULL == p_active_profile) {
        nsx_printf("as7058_get_active_profile returned NULL.\n");
        return ERR_CONFIG;
    }
    profile = *p_active_profile;
    spo2_profile_enabled = (profile.spo2_present && profile.spo2_enabled) ? true : false;

    g_spo2_config_valid = false;
    memset(&g_spo2_config, 0, sizeof(g_spo2_config));

    profile.control.reg_vals.i2c_mode = AS7058_USE_I2C ? 1 : 0;

    /* Match legacy sensor_configure(): force the LED-to-physical-position
     * mapping (led_sub1/led_sub2) to the board-profile macros (constants.h
     * AS7058_LED_SUB1_CFG/AS7058_LED_SUB2_CFG -- LED2/Red and LED3/IR on
     * the click board) whenever not using a dedicated SpO2 profile. This
     * is the fix for the "click_ppg_ecg" bring-up profile bug: that
     * profile's raw JSON data hardcoded led_sub1=1 (a stale/incorrect
     * physical LED index, not the click board's real Red LED position),
     * so only one (possibly wrong or unlit) LED ever fired. The active
     * profile is now as7058_get_active_profile() (AS7058_APP_PROFILE,
     * default CLICK_GOLDEN -- dual-wavelength Red+IR PPG + ECG, see
     * as7058_profiles.c) which already bakes in the correct mapping, but
     * this override is applied unconditionally (as legacy does) so it
     * stays correct regardless of which profile ends up selected. */
    if (!spo2_profile_enabled) {
        profile.led.reg_vals.led_sub1 = AS7058_LED_SUB1_CFG;
        profile.led.reg_vals.led_sub2 = AS7058_LED_SUB2_CFG;
    }

    result = as7058_apply_sensor_profile(&profile);
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_apply_sensor_profile returned error code %d.\n", result);
        return result;
    }

    if (spo2_profile_enabled) {
        g_spo2_config = profile.spo2_config;
        g_spo2_config_valid = true;
    }

    nsx_printf("AS7058 profile applied: ppg1_sub_en=%u ecg_subs=%u led_sub1=0x%02X led_sub2=0x%02X "
               "led1_ictrl=%u led2_ictrl=%u led3_ictrl=%u spo2=%d agc_channels=%u\n",
               profile.seq.reg_vals.ppg1_sub_en, profile.seq.reg_vals.ecg_subs,
               profile.led.reg_vals.led_sub1, profile.led.reg_vals.led_sub2,
               profile.led.reg_vals.led1_ictrl, profile.led.reg_vals.led2_ictrl,
               profile.led.reg_vals.led3_ictrl, (int)spo2_profile_enabled,
               (unsigned)profile.agc_config_num);

    return result;
}

bool
sensor_get_spo2_config(bio_spo2_a0_configuration_t *p_cfg)
{
    if (!g_spo2_config_valid || p_cfg == NULL) {
        return false;
    }
    *p_cfg = g_spo2_config;
    return true;
}

err_code_t
sensor_start(void)
{
    err_code_t result;
    as7058_meas_config_t meas_config;

    result = as7058_get_measurement_config(&meas_config);
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_get_measurement_config returned error code %d.\n", result);
        return result;
    }

    if (meas_config.ppg_sample_period_us > 0u) {
        nsx_printf("PPG sample period %lu us (~%lu Hz)\n", (uint32_t)meas_config.ppg_sample_period_us,
                   (uint32_t)(1000000u / meas_config.ppg_sample_period_us));
    }
    if (meas_config.ecg_seq1_sample_period_us > 0u) {
        nsx_printf("ECG seq1 sample period %lu us (~%lu Hz)\n", (uint32_t)meas_config.ecg_seq1_sample_period_us,
                   (uint32_t)(1000000u / meas_config.ecg_seq1_sample_period_us));
    }
    if (meas_config.ecg_seq2_sample_period_us > 0u) {
        nsx_printf("ECG seq2 sample period %lu us (~%lu Hz)\n", (uint32_t)meas_config.ecg_seq2_sample_period_us,
                   (uint32_t)(1000000u / meas_config.ecg_seq2_sample_period_us));
    }

    g_extract_metadata.copy_recent_to_current = FALSE;
    g_extract_metadata.fifo_map = meas_config.fifo_map;
    g_extract_metadata.current.ppg1_sub = 0;
    g_extract_metadata.current.ppg2_sub = 0;
    g_extract_metadata.recent.ppg1_sub = 0;
    g_extract_metadata.recent.ppg2_sub = 0;

    result = as7058_start_measurement(AS7058_MEAS_MODE_NORMAL);
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_start_measurement returned error code %d.\n", result);
        return result;
    }
    nsx_printf("AS7058 sensor_start OK (measurement running)\n");
    return result;
}

err_code_t
sensor_stop(void)
{
    err_code_t result;

    result = as7058_stop_measurement();
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_stop_measurement returned error code %d.\n", result);
        return result;
    }

    result = as7058_shutdown();
    if (result != ERR_SUCCESS) {
        nsx_printf("as7058_shutdown returned error code %d.\n", result);
        return result;
    }
    return result;
}

uint32_t
sensor_get_as7058_int_isr_count(void)
{
    return g_as7058_int_isr_count;
}

uint32_t
sensor_get_as7058_isr_min_interval_ms(void)
{
    uint32_t ticks = g_as7058_isr_min_interval_ticks;
    return (ticks == 0xFFFFFFFFu) ? 0u : (ticks * (1000u / configTICK_RATE_HZ));
}

uint32_t
sensor_get_as7058_isr_max_interval_ms(void)
{
    return g_as7058_isr_max_interval_ticks * (1000u / configTICK_RATE_HZ);
}

void
sensor_reset_as7058_isr_interval_stats(void)
{
    g_as7058_isr_min_interval_ticks = 0xFFFFFFFFu;
    g_as7058_isr_max_interval_ticks = 0;
}

uint32_t
sensor_get_irq_notify_missed_count(void)
{
    return g_sensor_irq_notify_missed;
}

uint32_t
sensor_get_ppg_push_count(void)
{
    return g_ppg_push_count;
}

uint32_t
sensor_get_ppg_drop_count(void)
{
    return g_ppg_drop_count;
}

uint32_t
sensor_get_ecg_push_count(void)
{
    return g_ecg_push_count;
}

uint32_t
sensor_get_ecg_drop_count(void)
{
    return g_ecg_drop_count;
}
