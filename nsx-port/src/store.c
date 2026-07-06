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

///////////////////////////////////////////////////////////////////////////////
// ECG Preprocess Configuration
///////////////////////////////////////////////////////////////////////////////

// print(pk.signal.generate_arm_biquad_sos(0.5, 30, 100, order=3, var_name="ecgSos"))
static float32_t ecgSosState[4 * ECG_SOS_LEN] = {0};
// {b0, b1, b2, a1, a2}
static float32_t ecgSos[5 * ECG_SOS_LEN] = {
   0.2467691808982006, 0.4935383617964012, 0.2467691808982006, -0.4141296048598937, -0.36229096617676754,
   1.0, 0.0, -1.0, 0.8213745394235588, 0.14232107570294283,
   1.0, -2.0, 1.0, 1.9684516644108876, -0.9694342914476478
};
arm_biquad_casd_df1_inst_f32 ecgFilterCtx = {.numStages = ECG_SOS_LEN, .pState = ecgSosState, .pCoeffs = ecgSos};

///////////////////////////////////////////////////////////////////////////////
// ECG Denoise Configuration
///////////////////////////////////////////////////////////////////////////////

float32_t ecgDenScratch[ECG_DEN_WINDOW_LEN];
float32_t ecgDenInout[ECG_DEN_WINDOW_LEN];

static float32_t ecgDenBuffer[ECG_DEN_BUF_LEN];
rb_config_t rbEcgDen = {
    .buffer = (void *)ecgDenBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_DEN_BUF_LEN,
    .head = 0,
    .tail = 0,
};

///////////////////////////////////////////////////////////////////////////////
// ECG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

float32_t ecgSegInout[ECG_SEG_WINDOW_LEN];
uint16_t ecgSegMask[ECG_SEG_WINDOW_LEN];

static float32_t ecgSegBuffer[ECG_SEG_BUF_LEN];
rb_config_t rbEcgSeg = {
    .buffer = (void *)ecgSegBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_SEG_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ecgPkPeakState[4 * ECG_SEG_WINDOW_LEN];
ecg_peak_f32_t ecgPkPeakCtx = {
    .qrsWin = 0.1,
    .avgWin = 1.0,
    .qrsPromWeight = 1.5,
    .qrsMinLenWeight = 0.4,
    .qrsDelayWin = 0.3,
    .sampleRate = 100,
    .state = ecgPkPeakState,
};

///////////////////////////////////////////////////////////////////////////////
// Shared Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

metrics_config_t metricsCfg = {};
uint32_t peaksMetrics[MAX_RR_PEAKS];
uint32_t rriMetrics[MAX_RR_PEAKS];
uint8_t rriMask[MAX_RR_PEAKS];

///////////////////////////////////////////////////////////////////////////////
// ECG Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

static float32_t ecgMetRBuffer[ECG_MET_BUF_LEN];
rb_config_t rbEcgMet = {
    .buffer = (void *)ecgMetRBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_MET_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static uint16_t ecgMaskMetRBuffer[ECG_MET_BUF_LEN];
rb_config_t rbEcgMaskMet = {
    .buffer = (void *)ecgMaskMetRBuffer,
    .dlen = sizeof(uint16_t),
    .size = ECG_MET_BUF_LEN,
    .head = 0,
    .tail = 0,
};

float32_t ecgMetData[ECG_MET_WINDOW_LEN];
uint16_t ecgMaskMetData[ECG_MET_WINDOW_LEN];

hrv_td_metrics_t ecgHrvMetrics;
metrics_ecg_results_t ecgMetResults = {
    .hr = 0,
    .hrv = 0,
    .denoiseCossim = 0,
    .arrhythmiaLabel = 0,
    .denoiseIps = 1,
    .segmentIps = 1,
    .arrhythmiaIps = 1,
    .qos = 0,
    .denoiseuIpspw = 1,
    .segmentuIpspw = 1,
    .arrhythmiaIpspw = 1,
};

///////////////////////////////////////////////////////////////////////////////
// PPG Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

static float32_t ppg1MetRBuffer[PPG_MET_BUF_LEN];
rb_config_t rbPpg1Met = {
    .buffer = (void *)ppg1MetRBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_MET_BUF_LEN,
    .head = 0,
    .tail = 0,
};

float32_t ppg1MetData[PPG_MET_WINDOW_LEN];

metrics_ppg_results_t ppgMetResults = {
    .pr = 0,
    .spo2 = 0,
    .qos = 0,
};

///////////////////////////////////////////////////////////////////////////////
// TileIO Streaming Taps
///////////////////////////////////////////////////////////////////////////////

static float32_t ecgTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgTx = {
    .buffer = (void *)ecgTxBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static uint16_t ecgMaskTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgMaskTx = {
    .buffer = (void *)ecgMaskTxBuffer,
    .dlen = sizeof(uint16_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ppg1TxBuffer[PPG_TX_BUF_LEN];
rb_config_t rbPpg1Tx = {
    .buffer = (void *)ppg1TxBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};
