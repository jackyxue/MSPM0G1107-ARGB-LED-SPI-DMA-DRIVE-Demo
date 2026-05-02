#include "debug.h"

#ifdef ENABLE_DEBUG

#include "ti_msp_dl_config.h" // 基礎配置
#include <ti/driverlib/dl_gpio.h>      // GPIO 功能
//#include <ti/driverlib/dl_iomux.h>     // 引腳複用功能 (重要！)[cite: 8]
#include "debug.h"
#include <stdio.h>
#include <stdarg.h>

// 定義緩衝區空間，128 bytes 對於一般的 Demo 訊息已經足夠
 char debug_buffer[128]; 

 


void UART_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    // 1. 直接格式化到全域緩衝區
    vsnprintf(debug_buffer, sizeof(debug_buffer), format, args);
    va_end(args);
    
    // 2. 直接在這裡進行 UART 傳輸，不再呼叫另一個帶 buffer 的 printf
    for (int i = 0; debug_buffer[i] != '\0'; i++) {
        DL_UART_Main_transmitDataBlocking(UART_0_INST, debug_buffer[i]);
    }
}
 


#endif