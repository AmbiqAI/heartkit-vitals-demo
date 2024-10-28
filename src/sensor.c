// Global includes
#include <arm_math.h>
// NS includes
#include "ns_ambiqsuite_harness.h"
#include "ns_spi.h"
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

static am_hal_gpio_pincfg_t g_osalGpioPincfg = {
    .GP.cfg_b.uFuncSel = AM_HAL_PIN_2_GPIO,
    .GP.cfg_b.eGPInput = AM_HAL_GPIO_PIN_INPUT_ENABLE,
    .GP.cfg_b.eGPRdZero = AM_HAL_GPIO_PIN_RDZERO_READPIN,
    .GP.cfg_b.eIntDir = AM_HAL_GPIO_PIN_INTDIR_LO2HI,
};

static volatile uint8_t g_spo2_ready_for_execution = 0;
static volatile uint8_t g_rrm_ready_for_execution = 0;
static volatile as7058_extract_metadata_t g_extract_metadata;
static sensor_context_t *g_sensorCtx;
static uint8_t g_led_current = 0;
static uint8_t g_pd_offset_current = 0;

///////////////////////////////////////////////////////////////////////////////
// OSAL Functions
///////////////////////////////////////////////////////////////////////////////

err_code_t
as7058_osal_int_pin_clear(void)
{
    uint32_t ui32IntStatus;

    AM_CRITICAL_BEGIN
    am_hal_gpio_interrupt_irq_status_get(GPIO0_001F_IRQn, false, &ui32IntStatus);
    am_hal_gpio_interrupt_irq_clear(GPIO0_001F_IRQn, ui32IntStatus);
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
    // Configure the GPIO pin.
    am_hal_gpio_pinconfig(ui32GpioNum, g_osalGpioPincfg);
    as7058_osal_int_pin_clear();
    // Enable the GPIO interrupt.
    am_hal_gpio_interrupt_control(AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_ENABLE, (void *)&ui32GpioNum);
    NVIC_SetPriority(GPIO0_001F_IRQn, AM_IRQ_PRIORITY_DEFAULT);
    NVIC_EnableIRQ(GPIO0_001F_IRQn);
    return ERR_SUCCESS;
}

err_code_t
as7058_osal_int_pin_deinit(void)
{
    uint32_t ui32GpioNum = AS7058_OSAL_INT_PIN;
    NVIC_DisableIRQ(GPIO0_001F_IRQn);
    as7058_osal_int_pin_clear();
    am_hal_gpio_interrupt_control(AM_HAL_GPIO_INT_CHANNEL_0, AM_HAL_GPIO_INT_CTRL_INDV_DISABLE, (void *)&ui32GpioNum);
    return ERR_SUCCESS;
}

void
am_gpio0_001f_isr(void)
{
    as7058_osal_int_pin_clear();
    as7058_osal_interrupt_callback();
}

///////////////////////////////////////////////////////////////////////////////
// AS7058 Callbacks
///////////////////////////////////////////////////////////////////////////////

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
                ringbuffer_push(&rbPpg1Sensor, &val_f32, 1);
            }
            else
            {
                val_f32 = ppg2_stimulus[stimulusIdx];
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
    float32_t ppgValue;
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
    result = as7058a_spo2_a0_set_input(
        p_fifo_data, fifo_data_size, sensor_events,
        p_agc_statuses, agc_statuses_num, &ready_for_execution);
    if (result != ERR_SUCCESS)
    {
        // ns_lp_printf("as7058a_spo2_a0_set_input returned error %d.\n", result);
        ns_lp_printf("x");
    }
    if (ready_for_execution)
    {
        g_spo2_ready_for_execution = ready_for_execution;
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
                ringbuffer_push(&rbEcgSensor, samples_f32, sample_cnt);
            }
            else
            {
                load_patient_data(sample_cnt, AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB1);
            }
        }
        // ECG (not used)
        else if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_ECG_SEQ1_SUB2)
        {
            ns_lp_printf("ECG Seq1 Sub2: %d\n", samples[0]);
        }
        // ECG (not used)
        else if (sub_sample_idx == AS7058_SUB_SAMPLE_ID_ECG_SEQ2_SUB1)
        {
            ns_lp_printf("ECG Seq2 Sub1: %d\n", samples[0]);
        }
    }
    // sensor_process_as7058_events(sensor_events);
}

err_code_t sensor_init(sensor_context_t *ctx)
{
    char *p_interface = NULL;
    err_code_t result = ERR_SUCCESS;
    uint8_t id;
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

    /* Configure the OS abstraction layer w/ SPI and interrupt pin */
    result = as7058_osal_configure(nsSpiCfg, as7058_osal_int_pin_read);
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
    err_code_t result = ERR_NOT_SUPPORTED;
    bio_spo2_a0_output_t spo2_output;
#if EN_SPO2_ALGO
    if (g_spo2_ready_for_execution)
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
    bio_rrm_a0_output_t rrm_output;
#if EN_RRM_ALGO
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
    err_code_t result;

    /* Configure register group POWER. */
    const as7058_reg_group_power_t power_config = {{
        .pwr_on = 23,
        .pwr_iso = 0,
        .clk_cfg = 7,
        .ref_cfg1 = 63,
        .ref_cfg2 = 14,
        .ref_cfg3 = 160,
        .standby_on1 = 0,
        .standby_on2 = 0,
        .standby_en1 = 4,
        .standby_en2 = 2,
        .standby_en3 = 4,
        .standby_en4 = 0,
        .standby_en5 = 3,
        .standby_en6 = 16,
        .standby_en7 = 16,
        .standby_en8 = 4,
        .standby_en9 = 0,
        .standby_en10 = 3,
        .standby_en11 = 16,
        .standby_en12 = 16,
        .standby_en13 = 16,
        .standby_en14 = 16,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_PWR, power_config.reg_buffer, sizeof(as7058_reg_group_power_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PWR returned error %d.\n", result);
        return result;
    }

    /* Configure register group CONTROL. */
    const as7058_reg_group_control_t control_config = {{
        .i2c_mode = 0,
        .int_cfg = 0,
        .if_cfg = 72,
        .gpio_cfg1 = 0,
        .gpio_cfg2 = 0,
        .io_cfg = 0,
    }};
    result =
        as7058_set_reg_group(AS7058_REG_GROUP_ID_CTRL, control_config.reg_buffer, sizeof(as7058_reg_group_control_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_CTRL returned error %d.\n", result);
        return result;
    }

    /* Configure register group LED. */
    const as7058_reg_group_led_t led_config = {{
        .vcsel_password = 87,
        .vcsel_cfg = 0,
        .vcsel_mode = 0,
        .led_cfg = 0,
        .led_drv1 = 0,
        .led_drv2 = 0,
        .led1_ictrl = 0,
        .led2_ictrl = 0,
        .led3_ictrl = 0,
        .led4_ictrl = 0,
        .led5_ictrl = 0,
        .led6_ictrl = 0,
        .led7_ictrl = 0,
        .led8_ictrl = 0,
        .led_irng1 = 255,
        .led_irng2 = 255,
        .led_sub1 = 34,
        .led_sub2 = 51,
        .led_sub3 = 0,
        .led_sub4 = 0,
        .led_sub5 = 0,
        .led_sub6 = 0,
        .led_sub7 = 0,
        .led_sub8 = 0,
        .lowvds_wait = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_LED, led_config.reg_buffer, sizeof(as7058_reg_group_led_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_LED returned error %d.\n", result);
        return result;
    }

    /* Configure register group PD. */
    const as7058_reg_group_pd_t pd_config = {{
        .pdsel_cfg = 0x00,
        .ppg1_pdsel1 = 2,
        .ppg1_pdsel2 = 2,
        .ppg1_pdsel3 = 2,
        .ppg1_pdsel4 = 0,
        .ppg1_pdsel5 = 0,
        .ppg1_pdsel6 = 0,
        .ppg1_pdsel7 = 0,
        .ppg1_pdsel8 = 0,
        .ppg2_pdsel1 = 0,
        .ppg2_pdsel2 = 0,
        .ppg2_pdsel3 = 0,
        .ppg2_pdsel4 = 0,
        .ppg2_pdsel5 = 0,
        .ppg2_pdsel6 = 0,
        .ppg2_pdsel7 = 0,
        .ppg2_pdsel8 = 0,
        .ppg2_afesel1 = 0,
        .ppg2_afesel2 = 0,
        .ppg2_afesel3 = 0,
        .ppg2_afesel4 = 0,
        .ppg2_afeen = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_PD, pd_config.reg_buffer, sizeof(as7058_reg_group_pd_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PD returned error %d.\n", result);
        return result;
    }

    /* Configure register group IOS. */
    const as7058_reg_group_ios_t ios_config = {{
        .ios_ppg1_sub1 = 0,
        .ios_ppg1_sub2 = 0,
        .ios_ppg1_sub3 = 0,
        .ios_ppg1_sub4 = 0,
        .ios_ppg1_sub5 = 0,
        .ios_ppg1_sub6 = 0,
        .ios_ppg1_sub7 = 0,
        .ios_ppg1_sub8 = 0,
        .ios_ppg2_sub1 = 0,
        .ios_ppg2_sub2 = 0,
        .ios_ppg2_sub3 = 0,
        .ios_ppg2_sub4 = 0,
        .ios_ppg2_sub5 = 0,
        .ios_ppg2_sub6 = 0,
        .ios_ppg2_sub7 = 0,
        .ios_ppg2_sub8 = 0,
        .ios_ledoff = 0,
        .ios_cfg = 0,
        .aoc_sar_thres = 0,
        .aoc_sar_range = 0,
        .aoc_sar_ppg1 = 0,
        .aoc_sar_ppg2 = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_IOS, ios_config.reg_buffer, sizeof(as7058_reg_group_ios_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_IOS returned error %d.\n", result);
        return result;
    }

    /* Configure register group PPG. */
    const as7058_reg_group_ppg_t ppg_config = {{
        .ppgmod_cfg1 = 0,
        .ppgmod_cfg2 = 0,
        .ppgmod_cfg3 = 0,
        .ppgmod1_cfg1 = 167,
        .ppgmod1_cfg2 = 100,
        .ppgmod1_cfg3 = 7,
        .ppgmod2_cfg1 = 39,
        .ppgmod2_cfg2 = 100,
        .ppgmod2_cfg3 = 7,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_PPG, ppg_config.reg_buffer, sizeof(as7058_reg_group_ppg_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PPG returned error %d.\n", result);
        return result;
    }

    /* Configure register group ECG. */
    const as7058_reg_group_ecg_t ecg_config = {{
        .bioz_cfg = 0,
        .bioz_excit = 0,
        .bioz_mixer = 0,
        .bioz_select = 13,
        .bioz_gain = 0,
        .ecgmod_cfg1 = 12,
        .ecgmod_cfg2 = 0,
        .ecgimux_cfg1 = 64,
        .ecgimux_cfg2 = 0,
        .ecgimux_cfg3 = 0,
        .ecgamp_cfg1 = 96,
        .ecgamp_cfg2 = 0,
        .ecgamp_cfg3 = 89,
        .ecgamp_cfg4 = 255,
        .ecgamp_cfg5 = 75,
        .ecgamp_cfg6 = 22,
        .ecgamp_cfg7 = 179,
        .ecg_bioz = 0,
        .leadoff_cfg = 0,
        .leadoff_thresl = 0,
        .leadoff_thresh = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_ECG, ecg_config.reg_buffer, sizeof(as7058_reg_group_ecg_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_ECG returned error %d.\n", result);
        return result;
    }

    /* Configure register group SINC. */
    const as7058_reg_group_sinc_t sinc_config = {{
        .ppg_sinc_cfga = 4,
        .ppg_sinc_cfgb = 3,
        .ppg_sinc_cfgc = 0,
        .ppg_sinc_cfgd = 0,
        .ecg1_sinc_cfga = 44,
        .ecg1_sinc_cfgb = 3,
        .ecg1_sinc_cfgc = 0,
        .ecg2_sinc_cfga = 44,
        .ecg2_sinc_cfgb = 3,
        .ecg2_sinc_cfgc = 0,
        .ecg_sinc_cfg = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_SINC, sinc_config.reg_buffer, sizeof(as7058_reg_group_sinc_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_SINC returned error %d.\n", result);
        return result;
    }

    /* Configure register group SEQ. */
    const as7058_reg_group_seq_t seq_config = {{
        .irq_enable = 150,
        .ppg_sub_wait = 50,
        .ppg_sar_wait = 0,
        .ppg_led_init = 25,
        .ppg_freql = 63,
        .ppg_freqh = 1,
        .ppg1_sub_en = 7,
        .ppg2_sub_en = 0,
        .ppg_mode_1 = 0,
        .ppg_mode_2 = 0,
        .ppg_mode_3 = 0,
        .ppg_mode_4 = 0,
        .ppg_mode_5 = 0,
        .ppg_mode_6 = 0,
        .ppg_mode_7 = 0,
        .ppg_mode_8 = 0,
        .ppg_cfg = 0,
        .ecg_freql = 39,
        .ecg_freqh = 0,
        .ecg1_freqdivl = 3,
        .ecg1_freqdivh = 0,
        .ecg2_freqdivl = 0,
        .ecg2_freqdivh = 0,
        .ecg_subs = 2,
        .leadoff_initl = 0,
        .leadoff_inith = 0,
        .ecg_initl = 1,
        .ecg_inith = 0,
        .sample_num = 0,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_SEQ, seq_config.reg_buffer, sizeof(as7058_reg_group_seq_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_SEQ returned error %d.\n", result);
        return result;
    }

    /* Configure register group PP. */
    const as7058_reg_group_pp_t post_config = {{
        .pp_cfg = 0x00,
        .ppg1_pp1 = 0x00,
        .ppg1_pp2 = 0x00,
        .ppg2_pp1 = 0x00,
        .ppg2_pp2 = 0x00,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_PP, post_config.reg_buffer, sizeof(as7058_reg_group_pp_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PP returned error %d.\n", result);
        return result;
    }

    /* Configure register group FIFO. */
    const as7058_reg_group_fifo_t fifo_config = {{
        .fifo_threshold = 64,
        .fifo_ctrl = 16,
    }};
    result = as7058_set_reg_group(AS7058_REG_GROUP_ID_FIFO, fifo_config.reg_buffer, sizeof(as7058_reg_group_fifo_t));
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_FIFO returned error %d.\n", result);
        return result;
    }

    /* Configure the Automatic Gain Control (AGC) algorithm using a dual-channel configuration that is suitable for SpO2
     * measurements using the AS7058A EVK. */

    const agc_configuration_t agc_config[2] = {
        {
            .mode = AGC_MODE_DEFAULT,
            .led_control_mode = AGC_AMPL_CNTL_MODE_AUTO,
            .channel = AS7058_SUB_SAMPLE_ID_PPG1_SUB1,
            .led_current_min = 15,
            .led_current_max = 50,
            .rel_amplitude_min_x100 = 5,
            .rel_amplitude_max_x100 = 25,
            .rel_amplitude_motion_x100 = 50,
            .num_led_steps = 5,
            .threshold_min = PPG_AGC_MIN,
            .threshold_max = PPG_AGC_MAX,
        },
        {
            .mode = AGC_MODE_DEFAULT,
            .led_control_mode = AGC_AMPL_CNTL_MODE_AUTO,
            .channel = AS7058_SUB_SAMPLE_ID_PPG1_SUB2,
            .led_current_min = 10,
            .led_current_max = 30,
            .rel_amplitude_min_x100 = 5,
            .rel_amplitude_max_x100 = 25,
            .rel_amplitude_motion_x100 = 50,
            .num_led_steps = 5,
            .threshold_min = PPG_AGC_MIN,
            .threshold_max = PPG_AGC_MAX,
        },
    };
    result = as7058_set_agc_config(agc_config, 2);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058_set_agc_config returned error %d.\n", result);
        return result;
    }

#if EN_SPO2_ALGO
    /* Configure the SpO2 algorithm with PPG sub-sample location. */
    result = as7058a_spo2_a0_set_signal_routing(
        AS7058_SUB_SAMPLE_ID_PPG1_SUB1,
        AS7058_SUB_SAMPLE_ID_PPG1_SUB2,
        AS7058_SUB_SAMPLE_ID_PPG1_SUB3);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_spo2_a0_set_signal_routing returned error %d.\n", result);
        return result;
    }

    /* Configure the SpO2 algorithm with calibration parameters suitable for the AS7058A EVK. */
    bio_spo2_a0_configuration_t spo2_config = {
        .a = 0,
        .b = 3499,
        .c = 11493,
        .dc_comp_red = 2079,
        .dc_comp_ir = 2079,
    };
    result = as7058a_spo2_a0_configure(spo2_config);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_spo2_a0_configure returned error %d.\n", result);
        return result;
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

    // const as7058_reg_group_iir_t iir_config = {{
    //     .iir_cfg = 11,
    //     .iir_coeff_data_sos = {
    //         // b0, b1, -a1, b2, -a2
    //         {9853, -18570, 28987, 9853, -14449},
    //         {16384, -30880, 28888, 16384, -14493},
    //         {16384, -30880, 29186, 16384, -14515},
    //         {16384, -30880, 28909, 16384, -14657},
    //         {16384, -30880, 29461, 16384, -14677},
    //         {16384, -30880, 29789, 16384, -14916},
    //         {16384, -30880, 29064, 16384, -14942},
    //         {16384, -30880, 30149, 16384, -15216},
    //         {16384, -30880, 29358, 16384, -15338},
    //         {16384, -30880, 30527, 16384, -15563},
    //         {16384, -30880, 29802, 16384, -15835},
    //         {16384, -30880, 30915, 16384, -15958},
    //     }
    // }};
    // result = as7058_set_reg_group(AS7058_REG_GROUP_ID_IIR, iir_config.reg_buffer, sizeof(as7058_reg_group_iir_t));
    // if (result != ERR_SUCCESS)
    // {
    //     ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_IIR returned error %d.\n", result);
    //     return result;
    // }

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

    /* Mark sub-sample w/ ID so extraction routine knows the FIFO structure */
    g_extract_metadata.copy_recent_to_current = FALSE;
    g_extract_metadata.fifo_map = meas_config.fifo_map;
    g_extract_metadata.current.ppg1_sub = 0;
    g_extract_metadata.current.ppg2_sub = 0;
    g_extract_metadata.recent.ppg1_sub = 0;
    g_extract_metadata.recent.ppg1_sub = 0;

#if EN_SPO2_ALGO
    /* Start the SpO2 processing */
    result = as7058a_spo2_a0_start_processing(meas_config);
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_spo2_a0_start_processing returned error %d.\n", result);
        return result;
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
    result = as7058a_spo2_a0_stop_processing();
    if (result != ERR_SUCCESS)
    {
        ns_lp_printf("as7058a_spo2_a0_stop_processing returned error %d.\n", result);
        return result;
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
