/**
 * @file main.c
 * @brief Phase 2 bring-up: AS7058 PPG+ECG raw sensing on apollo510_evb.
 *
 * Ported from legacy heartkit-vitals-demo. Validates the AS7058 sensor,
 * transport (I2C/SPI per AS7058_BOARD_PROFILE), and IRQ/ringbuffer plumbing
 * on physical hardware before layering DSP (nsx-physiokit), ML (heliaRT),
 * and streaming (nsx-tileio) on top (see plan phases 3+).
 */
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "nsx_core.h"
#include "nsx_freertos.h"
#include "nsx_i2c.h"
#include "nsx_power.h"
#include "nsx_spi.h"

#include "constants.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "store.h"

static TaskHandle_t sensorIrqTaskHandle;
static TaskHandle_t reportTaskHandle;

void
SensorIrqTask(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sensor_process_irq_events();
    }
}

/* Drains both sensor ringbuffers and reports throughput every second — the
 * phase-2 acceptance signal is non-zero, steadily incrementing push counts
 * with zero/near-zero drop counts on physical hardware. */
void
ReportTask(void *pvParameters)
{
    (void)pvParameters;
    float scratch[64];

    while (true) {
        while (ringbuffer_len(&rbPpg1Sensor) >= 32) {
            ringbuffer_pop(&rbPpg1Sensor, scratch, 32);
        }
        while (ringbuffer_len(&rbEcgSensor) >= 32) {
            ringbuffer_pop(&rbEcgSensor, scratch, 32);
        }

        nsx_printf("[sensor] isr=%lu missed=%lu ppg(push=%lu drop=%lu) ecg(push=%lu drop=%lu)\n",
                   (unsigned long)sensor_get_as7058_int_isr_count(),
                   (unsigned long)sensor_get_irq_notify_missed_count(),
                   (unsigned long)sensor_get_ppg_push_count(), (unsigned long)sensor_get_ppg_drop_count(),
                   (unsigned long)sensor_get_ecg_push_count(), (unsigned long)sensor_get_ecg_drop_count());

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int
main(void)
{
    nsx_core_config_t core_cfg = {
        .api = &nsx_core_V1_0_0,
    };
    NSX_TRY(nsx_core_init(&core_cfg), "Core Init failed.\n");

    /* Enable ITM/SWO before the perf-mode switch (see ppg-codec-demo main.c
     * for the Apollo5-family DCU/HFRC ordering rationale). */
    nsx_itm_printf_enable();

    NSX_TRY(nsx_power_configure(&nsxPwrCfg) != NSX_STATUS_SUCCESS, "Power Init failed.\n");
    nsx_delay_us(200000);

#if AS7058_USE_SPI
    NSX_TRY(nsx_spi_interface_init(&nsxSpiCfg, AM_HAL_IOM_2MHZ, AM_HAL_IOM_SPI_MODE_2) != NSX_STATUS_SUCCESS,
            "SPI Init Failed\n");
#else
    NSX_TRY(nsx_i2c_interface_init(&nsxI2cCfg, AS7058_I2C_SPEED_HZ) != NSX_STATUS_SUCCESS, "I2C Init Failed\n");
#endif

    NSX_TRY(sensor_init(&sensorCtx) != ERR_SUCCESS, "Sensor Init failed.\n");
    NSX_TRY(sensor_configure() != ERR_SUCCESS, "Sensor Configure failed.\n");

    NSX_TRY((xTaskCreate(
               SensorIrqTask,
               "SensorIrqTask",
               AS7058_SENSOR_TASK_STACK_WORDS,
               0,
               AS7058_SENSOR_TASK_PRIORITY,
               &sensorIrqTaskHandle) != pdPASS),
           "SensorIrqTask create failed.\n");

    sensor_set_irq_task_handle(sensorIrqTaskHandle);
    NSX_TRY(sensor_start() != ERR_SUCCESS, "Sensor Start failed.\n");

    NSX_TRY((xTaskCreate(
               ReportTask,
               "ReportTask",
               1024,
               0,
               1,
               &reportTaskHandle) != pdPASS),
           "ReportTask create failed.\n");

    nsx_printf("nsx-port phase 2: AS7058 PPG+ECG raw sensing bring-up\n");

    nsx_freertos_start();

    while (1) {}
}

/* configUSE_MALLOC_FAILED_HOOK == 1 requires this application hook. */
void
vApplicationMallocFailedHook(void)
{
    nsx_printf("nsx-port: malloc failed\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* configCHECK_FOR_STACK_OVERFLOW != 0 requires this application hook. */
void
vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    nsx_printf("nsx-port: stack overflow in %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
