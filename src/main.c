#include "ns_ambiqsuite_harness.h"
#include "ns_peripherals_power.h"
#include "main.h"

#if (configAPPLICATION_ALLOCATED_HEAP == 1)
    #define APP_HEAP_SIZE (8 * 4 * 1024)
size_t ucHeapSize = APP_HEAP_SIZE;
uint8_t ucHeap[APP_HEAP_SIZE] __attribute__((aligned(4)));
#endif

ns_power_config_t nsPwrCfg = {
    .api = &ns_power_V1_0_0,
    .eAIPowerMode = NS_MAXIMUM_PERF,
    .bNeedAudAdc = false,
    .bNeedSharedSRAM = true,
    .bNeedCrypto = true,
    .bNeedBluetooth = false,
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

int main(void) {

    NS_TRY(ns_core_init(&nsCoreCfg), "Core Init failed.\b");
    NS_TRY(ns_power_config(&nsPwrCfg), "Power Init Failed\n");

    ns_itm_printf_enable();
    ns_interrupt_master_enable();

    while (1) {
        ns_delay_us(200000); // 200ms
        ns_lp_printf("Hello World\n");
    }

}
