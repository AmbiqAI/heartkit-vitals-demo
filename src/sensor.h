#ifndef __APP_SENSOR_H
#define __APP_SENSOR_H

#include "FreeRTOS.h"
#include "task.h"
#include "error_codes.h"
#include "bio_spo2_a0_typedefs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initialized;
    uint8_t inputSource;
} sensor_context_t;

err_code_t sensor_init(sensor_context_t *ctx);
err_code_t sensor_configure(void);
err_code_t sensor_start(void);
err_code_t sensor_stop(void);
uint32_t sensor_get_as7058_int_isr_count(void);
uint32_t sensor_get_irq_notify_missed_count(void);
void sensor_set_irq_task_handle(TaskHandle_t handle);
void sensor_notify_irq_from_isr(BaseType_t *p_higher_priority_task_woken);
void sensor_process_irq_events(void);

err_code_t
sensor_read_spo2(float32_t *spo2, float32_t *heart_rate, float32_t *quality);

err_code_t
sensor_read_rrm(float32_t *rr);

err_code_t
sensor_read_hr(float32_t *hr);

uint8_t
sensor_get_spo2_config(bio_spo2_a0_configuration_t *p_cfg);

#ifdef __cplusplus
}
#endif

#endif // __APP_SENSOR_H
