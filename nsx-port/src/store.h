/**
 * @file store.h
 * @brief Central store for the NSX bring-up app (phase 2: sensor-only).
 *
 * Minimal subset of the legacy heartkit-vitals-demo store.h — board/bus
 * configuration only. DSP/ML/metrics/streaming globals will be reintroduced
 * in later phases as those subsystems are ported.
 */
#ifndef __APP_STORE_H
#define __APP_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nsx_i2c.h"
#include "nsx_power.h"
#include "nsx_spi.h"

#include "constants.h"
#include "sensor.h"

extern nsx_power_config_t nsxPwrCfg;
extern nsx_i2c_config_t nsxI2cCfg;
extern nsx_spi_config_t nsxSpiCfg;

extern sensor_context_t sensorCtx;

#ifdef __cplusplus
}
#endif

#endif // __APP_STORE_H
