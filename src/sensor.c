// Global includes
#include <arm_math.h>
// NS includes
#include "ns_ambiqsuite_harness.h"
#include "ns_spi.h"
#include "FreeRTOS.h"
#include "task.h"
// AS7058 includes
#include "error_codes.h"
#include "as7058_chiplib.h"
#include "as7058_osal_chiplib.h"
#include "as7058_extract.h"
#include "as7058_interface.h"
#include "as7058a_spo2_a0.h"
#include "as7058a_rrm_a0.h"
// Modules
#include "pk_filter.h"
#include "pk_transform.h"
// Local includes
#include "constants.h"
#include "store.h"
#include "stimulus.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "as7058_profiles.h"

static am_hal_gpio_pincfg_t g_osalGpioPincfg = {
    .GP.cfg_b.uFuncSel = AM_HAL_PIN_2_GPIO,
    .GP.cfg_b.eGPInput = AM_HAL_GPIO_PIN_INPUT_ENABLE,
    .GP.cfg_b.eGPRdZero = AM_HAL_GPIO_PIN_RDZERO_READPIN,
    .GP.cfg_b.eIntDir = AM_HAL_GPIO_PIN_INTDIR_LO2HI,
};

static volatile uint8_t g_spo2_ready_for_execution = 0;
static volatile uint8_t g_rrm_ready_for_execution = 0;
static volatile as7058_extract_metadata_t g_extract_metadata;
static volatile uint32_t g_as7058_int_isr_count = 0;
static volatile uint32_t g_sensor_irq_notify_missed = 0;
static uint8_t g_spo2_profile_enabled = 0;
static bio_spo2_a0_configuration_t g_spo2_config_for_metrics = {0};
static uint8_t g_spo2_config_for_metrics_valid = 0;
static as7058_sub_sample_ids_t g_spo2_red_sub_sample = AS7058_SUB_SAMPLE_ID_DISABLED;
static as7058_sub_sample_ids_t g_spo2_ir_sub_sample = AS7058_SUB_SAMPLE_ID_DISABLED;
static as7058_sub_sample_ids_t g_spo2_ambient_sub_sample = AS7058_SUB_SAMPLE_ID_DISABLED;
static uint8_t g_spo2_fifo_copy[AS7058_FIFO_DATA_BUFFER_SIZE];
static TaskHandle_t g_sensor_irq_task_handle = NULL;
static sensor_context_t *g_sensorCtx;

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

///////////////////////////////////////////////////////////////////////////////
// OSAL Functions
///////////////////////////////////////////////////////////////////////////////

static uint32_t
as7058_osal_int_irqn_get(uint32_t gpio_num)
{
    if (gpio_num <= 31) {
        return GPIO0_001F_IRQn;
    } else if (gpio_num <= 63) {
        return GPIO0_203F_IRQn;
    } else if (gpio_num <= 95) {
        return GPIO0_405F_IRQn;
    } else if (gpio_num <= 127) {
        return GPIO0_607F_IRQn;
    } else if (gpio_num <= 159) {
        return GPIO0_809F_IRQn;
    } else if (gpio_num <= 191) {
        return GPIO0_A0BF_IRQn;
    } else if (gpio_num <= 223) {
        return GPIO0_C0DF_IRQn;
    }
    return GPIO0_E0FF_IRQn;
}

static uint32_t
as7058_osal_int_pin_funcsel_get(uint32_t gpio_num)
{
    if (gpio_num == 2) {
        return AM_HAL_PIN_2_GPIO;
    }
#ifdef AM_HAL_PIN_50_GPIO
    if (gpio_num == 50) {
        return AM_HAL_PIN_50_GPIO;
    }
#endif
    return AM_HAL_PIN_2_GPIO;
}

err_code_t
as7058_osal_int_pin_clear(void)
{
    uint32_t ui32IntStatus;
    uint32_t ui32GpioNum = AS7058_OSAL_INT_PIN;
    uint32_t ui32IrqNum = as7058_osal_int_irqn_get(ui32GpioNum);

    AM_CRITICAL_BEGIN
    am_hal_gpio_interrupt_irq_status_get(ui32IrqNum, false, &ui32IntStatus);
    am_hal_gpio_interrupt_irq_clear(ui32IrqNum, ui32IntStatus);
    AM_CRITICAL_END

    return ERR_SUCCESS;
}

err_code_t
as7058_osal_int_pin_read(uint8_t *p_state)
{
    err_code_t result;
    uint32_t ui32Value;
    uint32_t ui32GpioNum = AS7058_OSAL_INT_PIN;

    result = am_hal_gpio_state_read(ui32GpioNum, AM_HAL_GPIO_INPUT_READ, &ui32Value);
    if (ERR_SUCCESS == result)
    {
        *p_state = (uint8_t)ui32Value;
    }
    return result;
}

err_code_t
as7058_osal_int_pin_init(void)
{
    uint32_t ui32GpioNum = AS7058_OSAL_INT_PIN;
    uint32_t ui32IrqNum = as7058_osal_int_irqn_get(ui32GpioNum);
    uint32_t ui32Status;
    g_osalGpioPincfg.GP.cfg_b.uFuncSel = as7058_osal_int_pin_funcsel_get(ui32GpioNum);
    // Configure the GPIO pin.
    ui32Status = am_hal_gpio_pinconfig(ui32GpioNum, g_osalGpioPincfg);
    if (ui32Status != AM_HAL_STATUS_SUCCESS)
    {
        ns_lp_printf("AS7058 INT pin config failed: pin=%u status=%u\n", ui32GpioNum, ui32Status);
        return ERR_SYSTEM_CONFIG;
    }

    as7058_osal_int_pin_clear();
    // Enable the GPIO interrupt.
    ui32Status = am_hal_gpio_interrupt_control(AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_ENABLE,
                                               (void *)&ui32GpioNum);
    if (ui32Status != AM_HAL_STATUS_SUCCESS)
    {
        ns_lp_printf("AS7058 INT enable failed: pin=%u irq=%u status=%u\n", ui32GpioNum, ui32IrqNum, ui32Status);
        return ERR_SYSTEM_CONFIG;
    }

    NVIC_SetPriority(ui32IrqNum, AM_IRQ_PRIORITY_DEFAULT);
    NVIC_EnableIRQ(ui32IrqNum);
    ns_lp_printf("AS7058 INT configured: pin=%u irq=%u trigger=LO2HI\n", ui32GpioNum, ui32IrqNum);
    return ERR_SUCCESS;
}

err_code_t
as7058_osal_int_pin_deinit(void)
{
    uint32_t ui32GpioNum = AS7058_OSAL_INT_PIN;
    uint32_t ui32IrqNum = as7058_osal_int_irqn_get(ui32GpioNum);
    NVIC_DisableIRQ(ui32IrqNum);
    as7058_osal_int_pin_clear();
    am_hal_gpio_interrupt_control(AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_DISABLE, (void *)&ui32GpioNum);
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

void
am_gpio0_001f_isr(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    g_as7058_int_isr_count++;
    as7058_osal_int_pin_clear();
    sensor_notify_irq_from_isr(&xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void
am_gpio0_203f_isr(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    g_as7058_int_isr_count++;
    as7058_osal_int_pin_clear();
    sensor_notify_irq_from_isr(&xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

///////////////////////////////////////////////////////////////////////////////
// AS7058 Callbacks
///////////////////////////////////////////////////////////////////////////////

#if EN_AS7058_CB_DEBUG_LOGS
static void
sensor_process_as7058_events(as7058_status_events_t sensor_events)
{
    /* Print enabled and active interrupt status registers. */
    uint8_t status_available = FALSE;
    if (0 != sensor_events.status_seq)
    {
        ns_lp_printf("STATUS_SEQ:0x%02X ", sensor_events.status_seq);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_led)
    {
        ns_lp_printf("STATUS_LED:0x%02X ", sensor_events.status_led);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_asata)
    {
        ns_lp_printf("STATUS_ASATA:0x%02X ", sensor_events.status_asata);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_asatb)
    {
        ns_lp_printf("STATUS_ASATB:0x%02X ", sensor_events.status_asatb);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_vcsel)
    {
        ns_lp_printf("STATUS_VCSEL:0x%02X ", sensor_events.status_vcsel);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_vcsel_vss)
    {
        ns_lp_printf("STATUS_VCSEL_VSS:0x%02X ", sensor_events.status_vcsel_vss);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_vcsel_vdd)
    {
        ns_lp_printf("STATUS_VCSEL_VDD:0x%02X ", sensor_events.status_vcsel_vdd);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_leadoff)
    {
        ns_lp_printf("STATUS_LEADOFF:0x%02X ", sensor_events.status_leadoff);
        status_available = TRUE;
    }
    if (0 != sensor_events.status_iir)
    {
        ns_lp_printf("IIR interrupt:%d ", sensor_events.status_iir);
        status_available = TRUE;
    }
    if (status_available)
    {
        ns_lp_printf("\n");
    }
}
#endif

uint32_t
load_patient_data(uint32_t reqSamples, uint32_t slot)
{
    static size_t _stimulus_slot_idxs[20] = {0};
    uint32_t numSamples = reqSamples;
    uint32_t ptSel = g_sensorCtx->inputSource;
    float32_t val_f32;
    size_t ptStart = 0;
    size_t ptEnd = 0;
    size_t stimulusIdx = 0;
    for (size_t i = 0; i < numSamples; i++)
    {
        stimulusIdx = _stimulus_slot_idxs[slot];
        // ECG slot
        if (slot == AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1)
        {
            ptStart = ptSel > 0 ? PTS_ECG_DATA_LEN * (ptSel - 1) : 0;
            ptEnd = ptSel > 0 ? ptStart + PTS_ECG_DATA_LEN : (NUM_INPUT_PTS - 1) * PTS_ECG_DATA_LEN;
            if (stimulusIdx < ptStart || stimulusIdx >= ptEnd)
            {
                stimulusIdx = ptStart;
            }
            val_f32 = ecg_stimulus[stimulusIdx];
            ringbuffer_push(&rbEcgSensor, &val_f32, 1);
        }
        // PPG slot
        else if (slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB1 || slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB2)
        {
            ptStart = ptSel > 0 ? PTS_PPG_DATA_LEN * (ptSel - 1) : 0;
            ptEnd = ptSel > 0 ? ptStart + PTS_PPG_DATA_LEN : (NUM_INPUT_PTS - 1) * PTS_PPG_DATA_LEN;
            if (stimulusIdx < ptStart || stimulusIdx >= ptEnd)
            {
                stimulusIdx = ptStart;
            }
            if (slot == AS7058_SUB_SAMPLE_ID_PPG1_SUB1)
            {
                val_f32 = ppg1_stimulus[stimulusIdx];
                if (ptSel != LIVE_INPUT_MODE) {
                    val_f32 += ppg_stim_gaussian_noise(PPG_STIM_GAUSS_STD);
                }
                ringbuffer_push(&rbPpg1Sensor, &val_f32, 1);
            }
            else
            {
                val_f32 = ppg2_stimulus[stimulusIdx];
                if (ptSel != LIVE_INPUT_MODE) {
                    val_f32 += ppg_stim_gaussian_noise(PPG_STIM_GAUSS_STD);
                }
                ringbuffer_push(&rbPpg2Sensor, &val_f32, 1);
            }
        }
        stimulusIdx++;
        _stimulus_slot_idxs[slot] = stimulusIdx;
    }
    return numSamples;
}

static void
sensor_as7058_callback(
    err_code_t error,
    const uint8_t *p_fifo_data,
    uint16_t fifo_data_size,
    const agc_status_t *p_agc_statuses,
    uint8_t agc_statuses_num,
    as7058_status_events_t sensor_events,
    const void *p_cb_param)
{
    int sub_sample_idx;
    err_code_t result;
    uint32_t samples[48];
    float32_t samples_f32[48];
    uint16_t sample_cnt;
    uint8_t ready_for_execution = 0;
    int samples_index;

    M_UNUSED_PARAM(p_cb_param);

    /* Check if error occurred in the Chip Library. */
    if (error != ERR_SUCCESS)
    {
        ns_lp_printf("Received error code %d from Chip Library.\n", error);
        return;
    }

#if EN_SPO2_ALGO
    /* Pass data to the SpO2 library. */
    if (g_spo2_profile_enabled &&
        (g_sensorCtx != NULL) &&
        (g_sensorCtx->inputSource == LIVE_INPUT_MODE) &&
        (p_fifo_data != NULL) &&
        (fifo_data_size > 0)) {
        const agc_status_t *p_spo2_agc_statuses = (agc_statuses_num > 0u) ? p_agc_statuses : NULL;
        uint8_t spo2_agc_statuses_num = (p_spo2_agc_statuses != NULL) ? agc_statuses_num : 0u;
        const uint8_t *p_spo2_fifo_data = p_fifo_data;
        if (fifo_data_size <= sizeof(g_spo2_fifo_copy)) {
            memcpy(g_spo2_fifo_copy, p_fifo_data, fifo_data_size);
            p_spo2_fifo_data = g_spo2_fifo_copy;
        }
        result = as7058a_spo2_a0_set_input(
            p_spo2_fifo_data, fifo_data_size, sensor_events,
            p_spo2_agc_statuses, spo2_agc_statuses_num, &ready_for_execution);
        if (result != ERR_SUCCESS) {
            ns_lp_printf("as7058a_spo2_a0_set_input returned error %d.\n", result);
        } else if (ready_for_execution) {
            g_spo2_ready_for_execution = ready_for_execution;
        }
    } else if ((g_sensorCtx != NULL) && (g_sensorCtx->inputSource != LIVE_INPUT_MODE)) {
        /* Non-live mode uses synthetic metrics path in main; prevent stale SpO2 execution. */
        g_spo2_ready_for_execution = 0;
    }
#endif

#if EN_RRM_ALGO
    /* Pass data to the RRM library. */
    result = as7058a_rrm_a0_set_input(p_fifo_data, fifo_data_size, sensor_events, p_agc_statuses, agc_statuses_num,
                                      NULL, 0, &ready_for_execution);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_rrm_a0_set_input returned error %d.\n", result);
        return;
    }
    if (ready_for_execution)
    {
        g_rrm_ready_for_execution = ready_for_execution;
    }
#endif

    /* Extract each sub-sample. */
    for (sub_sample_idx = AS7058_SUB_SAMPLE_ID_PPG1_SUB1; sub_sample_idx <= AS7058_SUB_SAMPLE_ID_ECG_SEQ2_SUB1; sub_sample_idx++)
    {
        sample_cnt = sizeof(samples) / sizeof(samples[0]);
        result = as7058_extract_samples(
            sub_sample_idx,
            p_fifo_data,
            fifo_data_size,
            samples,
            &sample_cnt,
            (as7058_extract_metadata_t *)&g_extract_metadata);

        if (result != ERR_SUCCESS || sample_cnt == 0)
        {
            continue;
        }

        for (samples_index = 0; samples_index < sample_cnt; samples_index++)
        {
            samples_f32[samples_index] = samples[samples_index];
            if ((sub_sample_idx == AS7058_SUB_SAMPLE_ID_PPG1_SUB1) || (sub_sample_idx == AS7058_SUB_SAMPLE_ID_PPG1_SUB2))
            {
                samples_f32[samples_index] = CLIP(samples_f32[samples_index], PPG_AGC_MIN, PPG_AGC_MAX);
                samples_f32[samples_index] -= PPG_AGC_MIN;
                samples_f32[samples_index] /= 16;
            }
        }

        // RED LED
        if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_PPG1_SUB1)
        {
            if (g_sensorCtx->inputSource == LIVE_INPUT_MODE)
            {
                // arm_biquad_cascade_df1_f32(&ppg1FilterCtx, samples_f32, samples_f32, sample_cnt);
                // Producer ownership: SensorIrqTask is the sole writer of sensor ringbuffers.
                ringbuffer_push(&rbPpg1Sensor, samples_f32, sample_cnt);
            }
            else
            {
                load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_PPG1_SUB1);
            }
        }
        // IR LED
        else if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_PPG1_SUB2)
        {
            // arm_biquad_cascade_df1_f32(&ppg2FilterCtx, samples_f32, samples_f32, sample_cnt);
            if (g_sensorCtx->inputSource == LIVE_INPUT_MODE)
            {
                // Producer ownership: SensorIrqTask is the sole writer of sensor ringbuffers.
                ringbuffer_push(&rbPpg2Sensor, samples_f32, sample_cnt);
            }
            else
            {
                load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_PPG1_SUB2);
            }
        }
        // Ambient (not used)
        else if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_PPG1_SUB3)
        {
        }
        // ECG
        else if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1)
        {
            if (g_sensorCtx->inputSource == LIVE_INPUT_MODE)
            {
                // Producer ownership: SensorIrqTask is the sole writer of sensor ringbuffers.
                ringbuffer_push(&rbEcgSensor, samples_f32, sample_cnt);
            }
            else
            {
                load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1);
            }
        }
        // ECG sub-slots not used by the current app path.
    }
}

err_code_t sensor_init(sensor_context_t *ctx)
{
    char *p_interface = NULL;
    err_code_t result = ERR_SUCCESS;
    as7058_osal_config_t osal_cfg = {0};
    g_sensorCtx = ctx;

    // arm_biquad_cascade_df1_init_f32(&ppg1FilterCtx, PPG_SOS_LEN, ppgSos, ppg1SosState);
    // arm_biquad_cascade_df1_init_f32(&ppg2FilterCtx, PPG_SOS_LEN, ppgSos, ppg2SosState);

    /* Initialize the interrupt pin */
    result = as7058_osal_int_pin_init();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_osal_int_pin_init returned error code %d.\n", result);
        return result;
    }

    /* Configure the OS abstraction layer w/ transport and interrupt pin */
#if AS7058_USE_SPI
    osal_cfg.bus = AS7058_OSAL_BUS_SPI;
    osal_cfg.p_spi_cfg = &nsSpiCfg;
#else
    osal_cfg.bus = AS7058_OSAL_BUS_I2C;
    osal_cfg.p_i2c_cfg = &nsI2cCfg;
    osal_cfg.i2c_addr = AS7058_I2C_ADDR;
#endif
    osal_cfg.read_pin_state = as7058_osal_int_pin_read;
    result = as7058_osal_configure(&osal_cfg);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_osal_configure returned error code %d.\n", result);
        return result;
    }

    /* Initialize the chip library */
    result = as7058_initialize(sensor_as7058_callback, NULL, NULL, p_interface);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_initialize returned error code %d.\n", result);
        return result;
    }

    /* Initialize the SpO2 algorithm */
#if EN_SPO2_ALGO
    result = as7058a_spo2_a0_initialize();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_spo2_a0_initialize returned error %d.\n", result);
        return result;
    }
#endif

#if EN_RRM_ALGO
    /* Initialize the RRM algorithm */
    result = as7058a_rrm_a0_initialize();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_rrm_a0_initialize returned error %d.\n", result);
        return result;
    }
#endif

    return result;
}

err_code_t
sensor_read_spo2(float32_t *spo2, float32_t *heart_rate, float32_t *quality)
{
    err_code_t result = g_spo2_profile_enabled ? ERR_NO_DATA : ERR_NOT_SUPPORTED;
    bio_spo2_a0_output_t spo2_output;
#if EN_SPO2_ALGO
    if (!g_spo2_profile_enabled) {
        return ERR_NOT_SUPPORTED;
    }
    if (!g_spo2_ready_for_execution) {
        return ERR_NO_DATA;
    }

    if (g_spo2_profile_enabled && g_spo2_ready_for_execution)
    {
        /* Execute the SpO2 algorithm. */
        result = as7058a_spo2_a0_execute();
        g_spo2_ready_for_execution = 0;
        /* Output is available from the SpO2 library. */
        if (ERR_SUCCESS == result)
        {
            result = as7058a_spo2_a0_get_output(&spo2_output);
            if (result == ERR_SUCCESS && !spo2_output.status)
            {
                *spo2 = spo2_output.spo2 / 100.0f;
                *heart_rate = spo2_output.heart_rate / 10.0f;
                *quality = spo2_output.quality;
            }
        }
    }
#endif
    return result;
}

err_code_t
sensor_read_rrm(float32_t *rr)
{
    err_code_t result = ERR_NOT_SUPPORTED;
#if EN_RRM_ALGO
    bio_rrm_a0_output_t rrm_output;
    if (g_rrm_ready_for_execution)
    {
        result = as7058a_rrm_a0_execute();
        g_rrm_ready_for_execution = 0;
        if (ERR_SUCCESS == result)
        {
            result = as7058a_rrm_a0_get_output(&rrm_output);
            if (result == ERR_SUCCESS)
            {
                *rr = rrm_output.respiratory_rate / 100.0f;
            }
        }
    }
#endif
    return result;
}

err_code_t
sensor_configure()
{
    const uint8_t spo2_led_half_scale = 31;
    err_code_t result;
    const as7058_sensor_profile_t *p_active_profile;
    as7058_sensor_profile_t profile;

    p_active_profile = as7058_get_active_profile();
    if (NULL == p_active_profile) {
        return ERR_CONFIG;
    }
    profile = *p_active_profile;
    g_spo2_profile_enabled = (profile.spo2_present && profile.spo2_enabled) ? 1 : 0;
    g_spo2_config_for_metrics_valid = 0;
    memset(&g_spo2_config_for_metrics, 0, sizeof(g_spo2_config_for_metrics));
    g_spo2_red_sub_sample = profile.spo2_red_sub_sample;
    g_spo2_ir_sub_sample = profile.spo2_ir_sub_sample;
    g_spo2_ambient_sub_sample = profile.spo2_ambient_sub_sample;

    profile.control.reg_vals.i2c_mode = AS7058_USE_I2C ? 1 : 0;
    if (!g_spo2_profile_enabled) {
        profile.led.reg_vals.led_sub1 = AS7058_LED_SUB1_CFG;
        profile.led.reg_vals.led_sub2 = AS7058_LED_SUB2_CFG;
    } else {
        /* If AGC is still disabled, use fixed LED current as a safe fallback. */
        if (profile.agc_config_num == 0) {
        if (profile.led.reg_vals.led2_ictrl == 0) {
            profile.led.reg_vals.led2_ictrl = spo2_led_half_scale;
        }
        if (profile.led.reg_vals.led3_ictrl == 0) {
            profile.led.reg_vals.led3_ictrl = spo2_led_half_scale;
        }
        ns_lp_printf("SpO2 fixed LED current applied: led2=%u led3=%u\n",
                     profile.led.reg_vals.led2_ictrl,
                     profile.led.reg_vals.led3_ictrl);
        }
    }

    result = as7058_apply_sensor_profile(&profile);
    if (result != ERR_SUCCESS)
    {
        return result;
    }

#if EN_SPO2_ALGO
    if (g_spo2_profile_enabled) {
        result = as7058a_spo2_a0_set_signal_routing(
            profile.spo2_red_sub_sample,
            profile.spo2_ir_sub_sample,
            profile.spo2_ambient_sub_sample);
        if (result != ERR_SUCCESS)
        {
            ns_lp_printf("as7058a_spo2_a0_set_signal_routing returned error %d.\n", result);
            return result;
        }

        result = as7058a_spo2_a0_configure(profile.spo2_config);
        if (result != ERR_SUCCESS)
        {
            ns_lp_printf("as7058a_spo2_a0_configure returned error %d.\n", result);
            return result;
        }
        g_spo2_config_for_metrics = profile.spo2_config;
        g_spo2_config_for_metrics_valid = 1;
    }
#endif

#if EN_RRM_ALGO
    /* Configure the RRM algorithm with PPG sub-sample location */
    result = as7058a_rrm_a0_set_signal_routing(AS7058_SUB_SAMPLE_ID_PPG1_SUB1);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_rrm_a0_set_signal_routing returned error %d.\n", result);
        return result;
    }
#endif

    return result;
}

err_code_t
sensor_start()
{
    /* Get the measurement configuration from the Chip Library. */
    err_code_t result;
    as7058_meas_config_t meas_config;
    result = as7058_get_measurement_config(&meas_config);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_get_measurement_config returned error code %d.\n", result);
        return result;
    }
    ns_lp_printf("PPG Sampling rate %d\n", meas_config.ppg_sample_period_us / 1000);
    ns_lp_printf("ECG Sampling rate %d\n", meas_config.ecg_seq1_sample_period_us / 1000);
    ns_lp_printf("ECG Sampling rate %d\n", meas_config.ecg_seq2_sample_period_us / 1000);

#if EN_SPO2_ALGO
    if (g_spo2_profile_enabled) {
        const uint32_t required_spo2_flags =
            M_AS7058_SUB_SAMPLE_ID_TO_FLAG(g_spo2_red_sub_sample) |
            M_AS7058_SUB_SAMPLE_ID_TO_FLAG(g_spo2_ir_sub_sample) |
            M_AS7058_SUB_SAMPLE_ID_TO_FLAG(g_spo2_ambient_sub_sample);
        ns_lp_printf("SpO2 routing: red=%u ir=%u amb=%u fifo_map=0x%08lX\n",
                     g_spo2_red_sub_sample, g_spo2_ir_sub_sample, g_spo2_ambient_sub_sample,
                     meas_config.fifo_map);
        if ((meas_config.fifo_map & required_spo2_flags) != required_spo2_flags) {
            ns_lp_printf("SpO2 routing mismatch: required flags 0x%08lX missing from fifo_map\n",
                         required_spo2_flags);
            return ERR_CONFIG;
        }
    }
#endif

    /* Mark sub-sample w/ ID so extraction routine knows the FIFO structure */
    g_extract_metadata.copy_recent_to_current = FALSE;
    g_extract_metadata.fifo_map = meas_config.fifo_map;
    g_extract_metadata.current.ppg1_sub = 0;
    g_extract_metadata.current.ppg2_sub = 0;
    g_extract_metadata.recent.ppg1_sub = 0;
    g_extract_metadata.recent.ppg2_sub = 0;

#if EN_SPO2_ALGO
    /* Start the SpO2 processing */
    if (g_spo2_profile_enabled) {
        result = as7058a_spo2_a0_start_processing(meas_config);
        if (result != ERR_SUCCESS)
        {
            ns_lp_printf("as7058a_spo2_a0_start_processing returned error %d.\n", result);
            return result;
        }
    }
#endif

#if EN_RRM_ALGO
    /* Start the RRM processing */
    result = as7058a_rrm_a0_start_processing(meas_config, 0);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_rrm_a0_start_processing returned error %d.\n", result);
        return result;
    }
#endif

    /* Start the measurement. */
    result = as7058_start_measurement(AS7058_MEAS_MODE_NORMAL);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_start_measurement returned error code %d.\n", result);
        return result;
    }
    return result;
}

err_code_t
sensor_stop()
{
    err_code_t result;

    /* Stop the measurement. */
    result = as7058_stop_measurement();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_stop_measurement returned error code %d.\n", result);
        return result;
    }

#if EN_SPO2_ALGO
    /* Stop the current processing session in the SpO2 library. */
    if (g_spo2_profile_enabled) {
        result = as7058a_spo2_a0_stop_processing();
        if (result != ERR_SUCCESS)
        {
            ns_lp_printf("as7058a_spo2_a0_stop_processing returned error %d.\n", result);
            return result;
        }
    }
#endif

#if EN_RRM_ALGO
    /* Stop the current processing session in the RRM library. */
    result = as7058a_rrm_a0_stop_processing();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_rrm_a0_stop_processing returned error %d.\n", result);
        return result;
    }
#endif

    /* Shutdown the sensor. */
    result = as7058_shutdown();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_shutdown returned error code %d.\n", result);
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

uint8_t
sensor_get_spo2_config(bio_spo2_a0_configuration_t *p_cfg)
{
    if ((p_cfg == NULL) || (g_spo2_config_for_metrics_valid == 0u)) {
        return 0;
    }
    *p_cfg = g_spo2_config_for_metrics;
    return 1;
}
