/* FreeRTOS configuration for the NCAPPS system (launcher, MIPS 24KEc at 648 MHz) */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configCPU_COUNT_HZ                      324000000UL     /* CP0 Count = CPU / 2 */
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                2048            /* words; U-Boot's ISR needs ~1 KB below sp */
#define configMAX_TASK_NAME_LEN                 12
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configTOTAL_HEAP_SIZE                   (1024 * 1024)   /* inside the launcher's 2 MB area */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0

#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TIMERS                        0

#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     1       /* sdk_timer_tick */
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2

#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetCurrentTaskHandle       1

void vAssertCalled(const char *file, int line);
#define configASSERT(x) do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

#endif
