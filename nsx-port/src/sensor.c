/**
 * @file sensor.c
 * @brief AS7058 PPG+ECG sensor bring-up (NSX port, phase 2: raw streaming only).
 *
 * Adapted from legacy heartkit-vitals-demo src/sensor.c and from the
 * ppg-codec-demo NSX reference port. GPIO/IRQ wiring and transport setup
 * follow the nsx-gpio / nsx-as7058_{i2c,spi} pattern established in
 * ppg-codec-demo; the AS7058 callback is extended here to extract both the
 * PPG1_SUB1 and ECG_SEQ1_SUB1 sub-samples (using the "click_ppg_ecg"
 * profile) instead of PPG-only.
 */
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
#include "generated/as7058_profile_click_ppg_ecg.h"
#include "ringbuffer.h"
#include "sensor.h"

#define SENSOR_RB_LEN (SENSOR_BUF_LEN)

static float32_t s_ppg1_rb_buf[SENSOR_RB_LEN];
static float32_t s_ecg_rb_buf[SENSOR_RB_LEN];

rb_config_t rbPpg1Sensor = {
    .buffer = s_ppg1_rb_buf, .dlen = sizeof(float32_t), .size = SENSOR_RB_LEN, .head = 0, .tail = 0,
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
    (void)pin;
    (void)ctx;

    g_as7058_int_isr_count++;
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

    // PPG1_SUB1 (green PPG channel)
    sample_cnt = (uint16_t)(sizeof(samples) / sizeof(samples[0]));
    result = as7058_extract_samples(AS7058_SUB_SAMPLE_ID_PPG1_SUB1, p_fifo_data, fifo_data_size, samples,
                                     &sample_cnt, (as7058_extract_metadata_t *)&g_extract_metadata);
    if (result == ERR_SUCCESS && sample_cnt > 0) {
        for (uint16_t i = 0; i < sample_cnt; i++) {
            samples_f32[i] = (float)samples[i];
        }
        pushed = ringbuffer_push(&rbPpg1Sensor, samples_f32, sample_cnt);
        g_ppg_push_count += (uint32_t)pushed;
        if (pushed < sample_cnt) {
            g_ppg_drop_count += (uint32_t)(sample_cnt - pushed);
        }
    }

    // ECG_SEQ1_SUB1 (ECG channel)
    sample_cnt = (uint16_t)(sizeof(samples) / sizeof(samples[0]));
    result = as7058_extract_samples(AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1, p_fifo_data, fifo_data_size, samples,
                                     &sample_cnt, (as7058_extract_metadata_t *)&g_extract_metadata);
    if (result == ERR_SUCCESS && sample_cnt > 0) {
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

    return result;
}

err_code_t
sensor_configure(void)
{
    err_code_t result;
    as7058_sensor_profile_t profile = g_as7058_profile_click_ppg_ecg;

    profile.control.reg_vals.i2c_mode = AS7058_USE_I2C ? 1 : 0;

    result = as7058_apply_sensor_profile(&profile);
    if (result != ERR_SUCCESS) {
        return result;
    }

    nsx_printf("AS7058 PPG+ECG bring-up profile applied: ppg1_sub_en=%u ecg_subs=%u\n",
               profile.seq.reg_vals.ppg1_sub_en, profile.seq.reg_vals.ecg_subs);

    return result;
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
