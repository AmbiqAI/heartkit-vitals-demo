/**
 * @file sensor.h
 * @brief AS7058 PPG+ECG sensor bring-up (NSX port, phase 2: raw streaming only).
 *
 * Ported from legacy heartkit-vitals-demo src/sensor.c, stripped down (no
 * store.c/metrics.c/tio_usb.h integration yet) to validate raw dual-channel
 * (PPG1_SUB1 + ECG_SEQ1_SUB1) streaming on physical apollo510_evb hardware
 * before wiring the full DSP/AI/streaming pipeline (see plan phases 3+).
 */
#ifndef __APP_SENSOR_H
#define __APP_SENSOR_H

#include "FreeRTOS.h"
#include "task.h"
#include "error_codes.h"

#include "ringbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t initialized;
} sensor_context_t;

err_code_t sensor_init(sensor_context_t *ctx);
err_code_t sensor_configure(void);
err_code_t sensor_start(void);
err_code_t sensor_stop(void);

uint32_t sensor_get_as7058_int_isr_count(void);
uint32_t sensor_get_irq_notify_missed_count(void);
uint32_t sensor_get_ppg_push_count(void);
uint32_t sensor_get_ppg_drop_count(void);
uint32_t sensor_get_ecg_push_count(void);
uint32_t sensor_get_ecg_drop_count(void);

void sensor_set_irq_task_handle(TaskHandle_t handle);
void sensor_notify_irq_from_isr(BaseType_t *p_higher_priority_task_woken);
void sensor_process_irq_events(void);

extern rb_config_t rbPpg1Sensor;
extern rb_config_t rbEcgSensor;

#ifdef __cplusplus
}
#endif

#endif // __APP_SENSOR_H
