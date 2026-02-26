#include "as7058_profiles.h"

#include "as7058_chiplib.h"
#include "constants.h"
#include "ns_ambiqsuite_harness.h"

#include "generated/as7058_profile_click_ppg_ecg.h"
#include "generated/as7058_profile_click_spo2.h"

static uint8_t
led_mask_from_ictrl(const as7058_reg_group_led_t *p_led)
{
    uint8_t mask = 0;

    if (p_led->reg_vals.led1_ictrl) { mask |= (1u << 0); }
    if (p_led->reg_vals.led2_ictrl) { mask |= (1u << 1); }
    if (p_led->reg_vals.led3_ictrl) { mask |= (1u << 2); }
    if (p_led->reg_vals.led4_ictrl) { mask |= (1u << 3); }
    if (p_led->reg_vals.led5_ictrl) { mask |= (1u << 4); }
    if (p_led->reg_vals.led6_ictrl) { mask |= (1u << 5); }
    if (p_led->reg_vals.led7_ictrl) { mask |= (1u << 6); }
    if (p_led->reg_vals.led8_ictrl) { mask |= (1u << 7); }

    return mask;
}

static err_code_t
validate_profile_for_board(const as7058_sensor_profile_t *p_profile)
{
    uint8_t led_sub_mask;
    uint8_t led_ictrl_mask;
    uint8_t invalid_led_mask;
    uint8_t invalid_pd_mask;
    const as7058_reg_group_led_t *p_led = &(p_profile->led);
    const as7058_reg_group_pd_t *p_pd = &(p_profile->pd);

    led_sub_mask = p_led->reg_vals.led_sub1 | p_led->reg_vals.led_sub2 | p_led->reg_vals.led_sub3 | p_led->reg_vals.led_sub4 |
                   p_led->reg_vals.led_sub5 | p_led->reg_vals.led_sub6 | p_led->reg_vals.led_sub7 | p_led->reg_vals.led_sub8;
    led_ictrl_mask = led_mask_from_ictrl(p_led);
    invalid_led_mask = (led_sub_mask | led_ictrl_mask) & (uint8_t)(~AS7058_BOARD_ALLOWED_LED_MASK);
    if (invalid_led_mask) {
        ns_lp_printf("AS7058 profile invalid for board: LED mask 0x%02X not allowed (allowed=0x%02X)\n",
                     invalid_led_mask, AS7058_BOARD_ALLOWED_LED_MASK);
        return ERR_CONFIG;
    }

    invalid_pd_mask = 0;
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel1 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel2 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel3 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel4 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel5 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel6 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel7 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg1_pdsel8 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel1 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel2 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel3 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel4 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel5 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel6 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel7 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    invalid_pd_mask |= p_pd->reg_vals.ppg2_pdsel8 & (uint8_t)(~AS7058_BOARD_ALLOWED_PD_MASK);
    if (invalid_pd_mask) {
        ns_lp_printf("AS7058 profile invalid for board: PD mask 0x%02X not allowed (allowed=0x%02X)\n",
                     invalid_pd_mask, AS7058_BOARD_ALLOWED_PD_MASK);
        return ERR_CONFIG;
    }

    return ERR_SUCCESS;
}

static const as7058_sensor_profile_t g_as7058_profile_click_golden = {
    .power = {{
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
    }},
    .control = {{
        .i2c_mode = 0,
        .int_cfg = 0,
        .if_cfg = 72,
        .gpio_cfg1 = 0,
        .gpio_cfg2 = 0,
        .io_cfg = 0,
    }},
    .led = {{
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
        .led_sub1 = AS7058_LED_SUB1_CFG,
        .led_sub2 = AS7058_LED_SUB2_CFG,
        .led_sub3 = 0,
        .led_sub4 = 0,
        .led_sub5 = 0,
        .led_sub6 = 0,
        .led_sub7 = 0,
        .led_sub8 = 0,
        .lowvds_wait = 0,
    }},
    .pd = {{
        .pdsel_cfg = 0,
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
    }},
    .ios = {{
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
    }},
    .ppg = {{
        .ppgmod_cfg1 = 0,
        .ppgmod_cfg2 = 0,
        .ppgmod_cfg3 = 0,
        .ppgmod1_cfg1 = 167,
        .ppgmod1_cfg2 = 100,
        .ppgmod1_cfg3 = 7,
        .ppgmod2_cfg1 = 39,
        .ppgmod2_cfg2 = 100,
        .ppgmod2_cfg3 = 7,
    }},
    .ecg = {{
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
    }},
    .sinc = {{
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
    }},
    .iir = {{
        .iir_cfg = 0,
        .reserved = 0,
        .iir_coeff_data_sos = {{0}},
    }},
    .seq = {{
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
    }},
    .pp = {{
        .pp_cfg = 0,
        .ppg1_pp1 = 0,
        .ppg1_pp2 = 0,
        .ppg2_pp1 = 0,
        .ppg2_pp2 = 0,
    }},
    .fifo = {{
        .fifo_threshold = 64,
        .fifo_ctrl = 16,
    }},
    .iir_present = 0,
    .iir_enabled = 0,
    .spo2_present = 1,
    .spo2_enabled = 1,
    .spo2_red_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB1,
    .spo2_ir_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB2,
    .spo2_ambient_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB3,
    .spo2_config = {
        .a = 0,
        .b = 3499,
        .c = 11493,
        .dc_comp_red = 1045,
        .dc_comp_ir = 1045,
    },
    .agc_config = {
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
            .reserved = {0, 0, 0},
            .threshold_min = PPG_AGC_MIN,
            .threshold_max = PPG_AGC_MAX,
        },
        {
            .mode = AGC_MODE_DEFAULT,
            .led_control_mode = AGC_AMPL_CNTL_MODE_AUTO,
            .channel = AS7058_SUB_SAMPLE_ID_PPG1_SUB2,
            .led_current_min = 10,
            .led_current_max = 24,
            .rel_amplitude_min_x100 = 5,
            .rel_amplitude_max_x100 = 25,
            .rel_amplitude_motion_x100 = 50,
            .num_led_steps = 4,
            .reserved = {0, 0, 0},
            .threshold_min = PPG_AGC_MIN,
            .threshold_max = PPG_AGC_MAX,
        },
    },
    .agc_config_num = 2,
};

static const as7058_sensor_profile_t g_as7058_profile_legacy_default __attribute__((unused)) = {
    .power = {{
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
    }},
    .control = {{
        .i2c_mode = 0,
        .int_cfg = 0,
        .if_cfg = 72,
        .gpio_cfg1 = 0,
        .gpio_cfg2 = 0,
        .io_cfg = 0,
    }},
    .led = {{
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
        .led_sub1 = AS7058_LED_SUB1_CFG,
        .led_sub2 = AS7058_LED_SUB2_CFG,
        .led_sub3 = 0,
        .led_sub4 = 0,
        .led_sub5 = 0,
        .led_sub6 = 0,
        .led_sub7 = 0,
        .led_sub8 = 0,
        .lowvds_wait = 0,
    }},
    .pd = {{
        .pdsel_cfg = 0,
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
    }},
    .ios = {{
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
    }},
    .ppg = {{
        .ppgmod_cfg1 = 0,
        .ppgmod_cfg2 = 0,
        .ppgmod_cfg3 = 0,
        .ppgmod1_cfg1 = 167,
        .ppgmod1_cfg2 = 100,
        .ppgmod1_cfg3 = 7,
        .ppgmod2_cfg1 = 39,
        .ppgmod2_cfg2 = 100,
        .ppgmod2_cfg3 = 7,
    }},
    .ecg = {{
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
    }},
    .sinc = {{
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
    }},
    .iir = {{
        .iir_cfg = 0,
        .reserved = 0,
        .iir_coeff_data_sos = {{0}},
    }},
    .seq = {{
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
    }},
    .pp = {{
        .pp_cfg = 0,
        .ppg1_pp1 = 0,
        .ppg1_pp2 = 0,
        .ppg2_pp1 = 0,
        .ppg2_pp2 = 0,
    }},
    .fifo = {{
        .fifo_threshold = 64,
        .fifo_ctrl = 16,
    }},
    .iir_present = 0,
    .iir_enabled = 0,
    .spo2_present = 1,
    .spo2_enabled = 1,
    .spo2_red_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB1,
    .spo2_ir_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB2,
    .spo2_ambient_sub_sample = AS7058_SUB_SAMPLE_ID_PPG1_SUB3,
    .spo2_config = {
        .a = 0,
        .b = 3499,
        .c = 11493,
        .dc_comp_red = 2079,
        .dc_comp_ir = 2079,
    },
    .agc_config = {
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
            .reserved = {0, 0, 0},
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
            .reserved = {0, 0, 0},
            .threshold_min = PPG_AGC_MIN,
            .threshold_max = PPG_AGC_MAX,
        },
    },
    .agc_config_num = 2,
};

const as7058_sensor_profile_t *
as7058_get_active_profile(void)
{
#if AS7058_APP_PROFILE == AS7058_APP_PROFILE_CLICK_GOLDEN
    return &g_as7058_profile_click_golden;
#elif AS7058_APP_PROFILE == AS7058_APP_PROFILE_CLICK_PPG_ECG
    return &g_as7058_profile_click_ppg_ecg;
#elif AS7058_APP_PROFILE == AS7058_APP_PROFILE_CLICK_SPO2
    return &g_as7058_profile_click_spo2;
#else
    return &g_as7058_profile_legacy_default;
#endif
}

static err_code_t
apply_group(as7058_reg_group_ids_t group_id, const uint8_t *buffer, uint8_t size)
{
    return as7058_set_reg_group(group_id, buffer, size);
}

err_code_t
as7058_apply_sensor_profile(const as7058_sensor_profile_t *p_profile)
{
    err_code_t result;

    if (NULL == p_profile) {
        return ERR_POINTER;
    }

    result = validate_profile_for_board(p_profile);
    if (result != ERR_SUCCESS) {
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_PWR, p_profile->power.reg_buffer, sizeof(as7058_reg_group_power_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PWR returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_CTRL, p_profile->control.reg_buffer, sizeof(as7058_reg_group_control_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_CTRL returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_LED, p_profile->led.reg_buffer, sizeof(as7058_reg_group_led_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_LED returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_PD, p_profile->pd.reg_buffer, sizeof(as7058_reg_group_pd_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PD returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_IOS, p_profile->ios.reg_buffer, sizeof(as7058_reg_group_ios_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_IOS returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_PPG, p_profile->ppg.reg_buffer, sizeof(as7058_reg_group_ppg_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PPG returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_ECG, p_profile->ecg.reg_buffer, sizeof(as7058_reg_group_ecg_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_ECG returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_SINC, p_profile->sinc.reg_buffer, sizeof(as7058_reg_group_sinc_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_SINC returned error %d.\n", result);
        return result;
    }

    if (p_profile->iir_present && p_profile->iir_enabled && EN_AS7058_IIR) {
        result = apply_group(AS7058_REG_GROUP_ID_IIR, p_profile->iir.reg_buffer, sizeof(as7058_reg_group_iir_t));
        if (result != ERR_SUCCESS) {
            ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_IIR returned error %d.\n", result);
            return result;
        }
    }

    result = apply_group(AS7058_REG_GROUP_ID_SEQ, p_profile->seq.reg_buffer, sizeof(as7058_reg_group_seq_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_SEQ returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_PP, p_profile->pp.reg_buffer, sizeof(as7058_reg_group_pp_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_PP returned error %d.\n", result);
        return result;
    }

    result = apply_group(AS7058_REG_GROUP_ID_FIFO, p_profile->fifo.reg_buffer, sizeof(as7058_reg_group_fifo_t));
    if (result != ERR_SUCCESS) {
        ns_lp_printf("Writing register group AS7058_REG_GROUP_ID_FIFO returned error %d.\n", result);
        return result;
    }

    result = as7058_set_agc_config(p_profile->agc_config, p_profile->agc_config_num);
    if (result != ERR_SUCCESS) {
        ns_lp_printf("as7058_set_agc_config returned error %d.\n", result);
        return result;
    }

    return ERR_SUCCESS;
}
