#include <arm_math.h>
// neuralSPOT
#include "ns_spi.h"
#include "ns_i2c.h"
// Modules
#include "ina228.h"
#include "pk_hrv.h"
#include "pk_ecg.h"
#include "pk_ppg.h"
// Local
#include "pmic.h"
#include "constants.h"
#include "store.h"

///////////////////////////////////////////////////////////////////////////////
// EVB Configuration
///////////////////////////////////////////////////////////////////////////////

ns_power_config_t nsPwrCfg = {
    .api = &ns_power_V1_0_0,
    .eAIPowerMode = NS_MINIMUM_PERF,
    .bNeedAudAdc = false,
    .bNeedSharedSRAM = true,
    .bNeedCrypto = true,
    .bNeedBluetooth = true,
    .bNeedUSB = true,
    .bNeedIOM = true,
    .bNeedAlternativeUART = false,
    .b128kTCM = false,
    .bEnableTempCo = false,
    .bNeedITM = true,
    // .bNeedXtal = true
};

ns_core_config_t nsCoreCfg = {
    .api = &ns_core_V1_0_0
};

ns_i2c_config_t nsI2cCfg = {
    .api = &ns_i2c_V1_0_0,
    .iom = I2C_IOM
};


ns_spi_config_t nsSpiCfg = {
    .iom = SPI_IOM
};


ns_timer_config_t ecgTimerCfg = {
    .api = &ns_timer_V1_0_0,
    .timer = NS_TIMER_COUNTER,
    .enableInterrupt = false
};

ns_timer_config_t ppgTimerCfg = {
    .api = &ns_timer_V1_0_0,
    .timer = NS_TIMER_INTERRUPT,
    .enableInterrupt = false,
};

///////////////////////////////////////////////////////////////////////////////
// Sensor Configuration
///////////////////////////////////////////////////////////////////////////////

sensor_context_t sensorCtx = {
    .initialized = false,
    .inputSource = 0
};

static float32_t ecgSensorBuffer[SENSOR_BUF_LEN];
rb_config_t rbEcgSensor = {
    .buffer = (void *)ecgSensorBuffer,
    .dlen = sizeof(float32_t),
    .size = SENSOR_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ppg1SensorBuffer[SENSOR_BUF_LEN];
rb_config_t rbPpg1Sensor = {
    .buffer = (void *)ppg1SensorBuffer,
    .dlen = sizeof(float32_t),
    .size = SENSOR_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ppg2SensorBuffer[SENSOR_BUF_LEN];
rb_config_t rbPpg2Sensor = {
    .buffer = (void *)ppg2SensorBuffer,
    .dlen = sizeof(float32_t),
    .size = SENSOR_BUF_LEN,
    .head = 0,
    .tail = 0,
};


///////////////////////////////////////////////////////////////////////////////
// Preprocess Configuration
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
float32_t ecgDenNoise[ECG_DEN_WINDOW_LEN];

static float32_t ecgDenBuffer[ECG_DEN_BUF_LEN];
rb_config_t rbEcgDen = {
    .buffer = (void *)ecgDenBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_DEN_BUF_LEN,
    .head = 0,
    .tail = 0,
};


///////////////////////////////////////////////////////////////////////////////
// PPG Denoise Configuration
///////////////////////////////////////////////////////////////////////////////

static float32_t ppg1DenBuffer[PPG_DEN_BUF_LEN];
rb_config_t rbPpg1Den = {
    .buffer = (void *)ppg1DenBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_DEN_BUF_LEN,
    .head = 0,
    .tail = 0,
};

float32_t ppg1DenInout[PPG_DEN_WINDOW_LEN];
float32_t ppg2DenInout[PPG_DEN_WINDOW_LEN];


static float32_t ppg2DenBuffer[PPG_DEN_BUF_LEN];
rb_config_t rbPpg2Den = {
    .buffer = (void *)ppg2DenBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_DEN_BUF_LEN,
    .head = 0,
    .tail = 0,
};



///////////////////////////////////////////////////////////////////////////////
// ECG Arrhythmia Configuration
///////////////////////////////////////////////////////////////////////////////

float32_t ecgArrScratch[ECG_ARR_WINDOW_LEN];
float32_t ecgArrInout[ECG_ARR_WINDOW_LEN];

///////////////////////////////////////////////////////////////////////////////
// ECG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

float32_t ecgSegScratch[ECG_SEG_WINDOW_LEN];
float32_t ecgSegInout[ECG_SEG_WINDOW_LEN];
uint16_t ecgSegMask[ECG_SEG_WINDOW_LEN];

static float32_t ecgRawSegBuffer[ECG_SEG_BUF_LEN];
rb_config_t rbEcgRawSeg = {
    .buffer = (void *)ecgRawSegBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_SEG_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ecgSegBuffer[ECG_SEG_BUF_LEN];
rb_config_t rbEcgSeg = {
    .buffer = (void *)ecgSegBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_SEG_BUF_LEN,
    .head = 0,
    .tail = 0,
};

///////////////////////////////////////////////////////////////////////////////
// PPG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

float32_t ppg1SegInout[PPG_SEG_WINDOW_LEN];
float32_t ppg2SegInout[PPG_SEG_WINDOW_LEN];

static float32_t ppg1SegBuffer[PPG_SEG_BUF_LEN];
rb_config_t rbPpg1Seg = {
    .buffer = (void *)ppg1SegBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_SEG_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ppg2SegBuffer[PPG_SEG_BUF_LEN];
rb_config_t rbPpg2Seg = {
    .buffer = (void *)ppg2SegBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_SEG_BUF_LEN,
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
    .state = ecgPkPeakState
};

///////////////////////////////////////////////////////////////////////////////
// Shared Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

metrics_config_t metricsCfg = {};
uint32_t peaksMetrics[MAX_RR_PEAKS];
uint32_t rriMetrics[MAX_RR_PEAKS];
uint8_t rriMask[MAX_RR_PEAKS];
static float32_t pkArena[5*ECG_MET_BUF_LEN];

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
float32_t ecgDenMetData[ECG_MET_WINDOW_LEN];
uint16_t ecgMaskMetData[ECG_MET_WINDOW_LEN];

metrics_app_results_t appMetResults = {
    .cpuPercUtil = 0,
    .batteryDays = 0,
    .avgAiIps = 0,
};

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
    .arrhythmiaIpspw = 1
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

static float32_t ppg2MetRBuffer[PPG_MET_BUF_LEN];
rb_config_t rbPpg2Met = {
    .buffer = (void *)ppg2MetRBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_MET_BUF_LEN,
    .head = 0,
    .tail = 0,
};

float32_t ppg1MetData[PPG_MET_WINDOW_LEN];
float32_t ppg2MetData[PPG_MET_WINDOW_LEN];

metrics_ppg_results_t ppgMetResults = {
    .pr = 0,
    .spo2 = 0,
    .qos = 0
};


///////////////////////////////////////////////////////////////////////////////
// TileIO Configuration
///////////////////////////////////////////////////////////////////////////////

static float32_t ecgRawTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgRawTx = {
    .buffer = (void *)ecgRawTxBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ecgDenTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgDenTx = {
    .buffer = (void *)ecgDenTxBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

uint16_t ecgMaskTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgMaskTx = {
    .buffer = (void *)ecgMaskTxBuffer,
    .dlen = sizeof(uint16_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ecgCpuTxBuffer[ECG_TX_BUF_LEN];
rb_config_t rbEcgCpuTx = {
    .buffer = (void *)ecgCpuTxBuffer,
    .dlen = sizeof(float32_t),
    .size = ECG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t ppgCpuTxBuffer[PPG_TX_BUF_LEN];
rb_config_t rbPpgCpuTx = {
    .buffer = (void *)ppgCpuTxBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

static float32_t totalCpuTxBuffer[PPG_TX_BUF_LEN];
rb_config_t rbTotalCpuTx = {
    .buffer = (void *)totalCpuTxBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_TX_BUF_LEN,
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

static float32_t ppg2TxBuffer[PPG_TX_BUF_LEN];
rb_config_t rbPpg2Tx = {
    .buffer = (void *)ppg2TxBuffer,
    .dlen = sizeof(float32_t),
    .size = PPG_TX_BUF_LEN,
    .head = 0,
    .tail = 0,
};

app_state_t appState = {
    .inputSource = 1,
    .bwNoiseLevel = 0,
    .maNoiseLevel = 0,
    .emNoiseLevel = 0,
    .speedMode = 0,
    .denoiseMode = DenoiseModeAi,
    .segMode = SegmentationModeAi,
    .arrMode = ArrhythmiaModeAi,
};

///////////////////////////////////////////////////////////////////////////////
// PMIC Configuration
///////////////////////////////////////////////////////////////////////////////

pmic_metrics_results_t g_pmicMetrics = {
    .current = 0.0f,
    .voltage = 0.0f,
    .shunt_voltage = 0.0f,
    .power = 0.0f,
    .energy = 0.0f,
    .charge = 0.0f,
    .temperature = 0.0f,
    .cpu = 0.0f,
};

static inline uint32_t
ina228_write_read(uint16_t addr, const void *write_buf, size_t num_write, void *read_buf, size_t num_read) {
    return ns_i2c_write_read(&nsI2cCfg, addr, write_buf, num_write, read_buf, num_read);
}

static inline uint32_t
ina228_read(const void *buf, uint32_t num_bytes, uint16_t addr) {
    return ns_i2c_read(&nsI2cCfg, buf, num_bytes, addr);
}

static inline uint32_t
ina228_write(const void *buf, uint32_t num_bytes, uint16_t addr) {
    return ns_i2c_write(&nsI2cCfg, buf, num_bytes, addr);
}

ina228_context_t g_ina228Ctx = {
    .addr = 0x40,
    ._shunt_res = 0.0025,
    ._current_lsb = 0.0001,
    .i2c_write_read = &ina228_write_read,
    .i2c_read = &ina228_read,
    .i2c_write = &ina228_write
};


static uint8_t tioUsbTxBuffer[512*TIO_USB_PACKET_LEN];
rb_config_t rbTioUsbTx = {
    .buffer = (void *)tioUsbTxBuffer,
    .dlen = TIO_USB_PACKET_LEN,
    .size = 512,
    .head = 0,
    .tail = 0,
};
