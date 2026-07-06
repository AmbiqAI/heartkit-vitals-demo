#include "constants.h"
#include "store.h"

nsx_power_config_t nsxPwrCfg = {
    .api = &nsx_power_V1_0_0,
    .perf_mode = NSX_POWER_PERF_LOW,
    .need_audadc = false,
    .need_ssram = true,
    .need_crypto = true,
    .need_ble = true,
    .need_usb = true,
    .need_iom = true,
    .need_uart = false,
    .small_tcm = false,
    .need_tempco = false,
    .need_itm = true,
    .need_xtal = false,
    .spotmgr_collapse = false,
};

nsx_i2c_config_t nsxI2cCfg = {
    .api = &nsx_i2c_V1_0_0,
    .iom = I2C_IOM,
};

nsx_spi_config_t nsxSpiCfg = {
    .iom = SPI_IOM,
};

sensor_context_t sensorCtx = {
    .initialized = false,
};
