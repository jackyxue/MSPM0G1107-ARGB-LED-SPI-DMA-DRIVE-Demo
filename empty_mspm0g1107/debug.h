#ifndef _DEBUG_H_
#define _DEBUG_H_

#include "ti_msp_dl_config.h"
#include <stdio.h>
#include <stdarg.h>

// --- 全域偵錯開關 ---
#define ENABLE_DEBUG 

#ifdef ENABLE_DEBUG
 
 /* 檢查 SysConfig 是否正確配置了名為 UART_0 的實例 */
#ifndef UART_0_INST
    #error "********************************************************"
    #error "* [CONFIG ERROR]: UART_0 is missing in SysConfig!      *"
    #error "*                                                      *"
    #error "* ACTION REQUIRED:                                     *"
    #error "*  1. Open your .syscfg file                           *"
    #error "*  2. Add UART module -> Name it 'UART_0'              *"
    #error "*  3. [Hardware Trap]: Ensure UART_0 Pinmux is set to  *"
    #error "*     TX=PA10, RX=PA11. Using PA0/PA1 will NOT route   *"
    #error "*     data to the XDS110 USB COM Port!                 *"
    #error "********************************************************"
#else
    /* 如果 UART_0 已經存在，我們還是保留警告，以防開發者只加了模組卻忘了改引腳 */
    #warning "[Hardware Trap]: Double check your UART_0 Pinmux. TX must be PA10, RX must be PA11 for XDS110 USB COM Port!"
#endif
/* 如果你有特定的參數要求，也可以進一步檢查 */
#if (UART_0_BAUD_RATE != 115200)
    #warning "[Optimization]: Current baud rate is not 115200. Is this intended?"
#endif
  
	// 函數宣告
void UART_printf(const char *format, ...);

// 如果你想用更短的名字，可以保留這個宏
#define uprint(...) UART_printf(__VA_ARGS__)
	
#else
    // 關閉時，printf 直接變成無動作，不佔 Flash 也不佔 Stack[cite: 9]
    #define printf(fmt, ...) do {} while (0)
    #define UART_printf(fmt, ...) do {} while (0)
#endif

#endif // _DEBUG_H_