/**
 * @file pmic.c
 * @author Adam Page (adam.page@ambiq.com)
 * @brief
 * @version 0.1
 * @date 2024-10-04
 *
 * @copyright Copyright (c) 2024
 *
 */

#include "ns_ambiqsuite_harness.h"
#include "ina228.h"
#include "store.h"
#include "pmic.h"

float32_t g_current_offset = 0;

uint32_t
pmic_init(void)
{
    ina228_initialize(&g_ina228Ctx);
    ina228_set_adc_range(&g_ina228Ctx, 1); // 1 = 40.96mV
    ina228_set_shunt(&g_ina228Ctx, 0.016, 0.2); // 0.015 Ohm, 200 mA
    ina228_set_averaging_count(&g_ina228Ctx, INA228_COUNT_256);
    ina228_set_voltage_conversion_time(&g_ina228Ctx, INA228_TIME_280_us);
    ina228_set_current_conversion_time(&g_ina228Ctx, INA228_TIME_280_us);
    ina228_reset_accumulators(&g_ina228Ctx);
    return 0;
}

uint32_t
pmic_start(uint8_t en_dc_offset)
{
    if (en_dc_offset) {
        ina228_read_current(&g_ina228Ctx, &g_current_offset);
    } else {
        g_current_offset = 0;
    }
    return 0;
}

uint32_t
pmic_read_values(pmic_metrics_results_t *pmic)
{
    float32_t current, power;
    ina228_read_current(&g_ina228Ctx, &current);
    ina228_read_bus_voltage(&g_ina228Ctx, &pmic->voltage);
    ina228_read_shunt_voltage(&g_ina228Ctx, &pmic->shunt_voltage);
    ina228_read_power(&g_ina228Ctx, &power);
    ina228_read_energy(&g_ina228Ctx, &pmic->energy);
    ina228_read_die_temp(&g_ina228Ctx, &pmic->temperature);
    ina228_read_charge(&g_ina228Ctx, &pmic->charge);
    pmic->current = MAX(current - g_current_offset, 0);
    pmic->power = pmic->current*pmic->voltage/1000;
    return 0;
}

uint32_t
pmic_display_values(pmic_metrics_results_t *pmic)
{
    ns_lp_printf("Current: %0.2f mA\n", pmic->current);
    ns_lp_printf("Voltage: %0.2f mV\n", pmic->voltage);
    ns_lp_printf("Shunt Voltage: %0.2f mV\n", pmic->shunt_voltage);
    ns_lp_printf("Power: %0.2f mW\n", pmic->power);
    ns_lp_printf("Energy: %0.4f J\n", pmic->energy);
    ns_lp_printf("Charge: %0.4f C\n", pmic->charge);
    ns_lp_printf("Temperature: %0.2f C\n", pmic->temperature);
    return 0;
}
