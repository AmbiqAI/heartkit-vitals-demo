#ifndef __APP_SENSOR_H
#define __APP_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "error_codes.h"

typedef struct {
    uint8_t initialized;
    uint8_t inputSource;
} sensor_context_t;

err_code_t sensor_init(sensor_context_t *ctx);
err_code_t sensor_configure(void);
err_code_t sensor_start(void);
err_code_t sensor_stop(void);
uint32_t sensor_get_as7058_int_isr_count(void);

err_code_t
sensor_read_spo2(float32_t *spo2, float32_t *heart_rate, float32_t *quality);

err_code_t
sensor_read_rrm(float32_t *rr);

err_code_t
sensor_read_hr(float32_t *hr);

#ifdef __cplusplus
}
#endif

#endif // __APP_SENSOR_H
