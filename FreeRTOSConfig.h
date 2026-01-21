#pragma once

#include <stdint.h>

#define configUSE_PREEMPTION            1
#define configUSE_TIME_SLICING          1
#define configCPU_CLOCK_HZ              ( ( unsigned long ) 50000000 )
#define configTICK_RATE_HZ              ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES            5
#define configMINIMAL_STACK_SIZE        ( ( unsigned short ) 128 )
#define configTOTAL_HEAP_SIZE           ( ( size_t ) ( 12 * 1024 ) )
#define configMAX_TASK_NAME_LEN         16
#define configUSE_16_BIT_TICKS          0
#define configIDLE_SHOULD_YIELD         1

#define configUSE_MUTEXES               1
#define configUSE_COUNTING_SEMAPHORES   1
#define configQUEUE_REGISTRY_SIZE       0

#define configUSE_TRACE_FACILITY        0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configCHECK_FOR_STACK_OVERFLOW  0
#define configUSE_MALLOC_FAILED_HOOK    0

#define INCLUDE_vTaskDelay              1
#define INCLUDE_vTaskDelete             0
#define INCLUDE_vTaskSuspend            0
#define INCLUDE_xTaskGetSchedulerState  1

#define configKERNEL_INTERRUPT_PRIORITY         255
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    191

