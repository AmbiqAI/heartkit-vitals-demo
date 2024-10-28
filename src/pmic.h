#ifndef __APP_PMIC_H
#define __APP_PMIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "arm_math.h"


typedef struct {
    float32_t current;
    float32_t voltage;
    float32_t shunt_voltage;
    float32_t power;
    float32_t energy;
    float32_t charge;
    float32_t temperature;
    float32_t cpu;
} pmic_metrics_results_t;

uint32_t
pmic_init(void);

uint32_t
pmic_start(uint8_t en_dc_offset);

uint32_t
pmic_read_values(pmic_metrics_results_t *pmic);

uint32_t
pmic_display_values(pmic_metrics_results_t *pmic);



#ifdef __cplusplus
}
#endif

#endif // __APP_PMIC_H
