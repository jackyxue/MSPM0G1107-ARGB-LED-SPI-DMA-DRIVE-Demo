# MSPM0G1107-ARGB-LED-SPI-DMA-DRIVE-Demo

![License](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform](https://img.shields.io/badge/Platform-TI%20MSPM0-orange.svg)
![Version](https://img.shields.io/badge/Version-0.0.1-green.svg)

這是一個基於 **TI MSPM0G1107** (Cortex-M0+) 核心的高性能 ARGB LED 驅動專案。本專案透過 **SPI DMA** 技術實現了幾乎零 CPU 負擔的 WS2811 燈條驅動，並包含一套完整的特效管理框架與開發者友善的時間控制宏。

## 🌟 核心特色

*   **高效能 DMA 驅動**：利用 SPI DMA 與時序優化技術，精準控制 5V WS2811 燈條，確保在大規模 LED 驅動下系統依然流暢。
*   **工程師友善的時間管理**：
    *   **SECONDS(s) 宏**：允許直接以「秒」為單位設定展示時間，系統會根據 `SYSTEM_FPS` 自動換算為對應幀數。
    *   **可調 FPS 基準**：透過修改 `SYSTEM_FPS` 即可輕鬆適應不同硬體效能或延遲設定。
*   **專業級動態 Log 監控**：
    *   **原地更新顯示**：優化 UART 輸出邏輯，進度（Progress %）與運行時間（Time ms）在同一行更新，避免洗板並保持控制台整潔。
    *   **邏輯修正**：修正了循環重置導致進度無法精準顯示 100% 的邊界問題。
*   **21 種內建特效**：
    *   包含靜態七彩、Comet (彗星)、Rainbow Cycle (彩虹流動)、Scanner (掃描)。
    *   **科幻主題特效**：特別收錄以宇宙論為靈感的 **Mandela Glitch (曼德拉幻影)** 與 **Big Crunch (大擠壓)**。

## 🛠️ 硬體開發環境

*   **MCU**: TI MSPM0G1107 (LP-MSPM0G3507 可相容)
*   **LED**: 5V WS2811 ARGB LED Strip
*   **IDE**: Code Composer Studio (CCS) / tiarmclang 4.0.4.LTS
*   **通信協議**: SPI DMA @ High Speed

## 📂 代碼架構建議

為了提升可讀性與維護性，專案使用了具名列舉 (Enum) 與宏定義：
```c
// 1. 定義顏色索引 (Friendly for Engineers)
typedef enum {
    COLOR_IDX_RED = 0,
    COLOR_IDX_ORANGE,
    // ...
    COLOR_IDX_MAX
} LED_Color_Index;

// 2. 使用 SECONDS 宏快速設定展示時間
#define EFFECT_DEMO_SEC 60

const Effect_Type EFFECT_TABLE[] = {
    [COLOR_IDX_RED] = {"Red Static", SECONDS(10), mode_static_rainbow},
    {"Mandela Glitch", SECONDS(EFFECT_DEMO_SEC), effect_mandela_glitch},
    {"Rainbow Flow",   SECONDS(EFFECT_DEMO_SEC), effect_rainbow_cycle}
};

📊 偵錯資訊輸出範例
系統會透過 UART 即時反饋當前狀態，優化後的格式如下（支援 ANSI 終端控制碼）：

Plaintext
[MODE] #8: Rainbow Flow
  >> Time: 60012 ms | Progress: 100% [DONE]

[MODE] #9: Mandela Glitch
  >> Time: 15420 ms | Progress: 25% 
🚀 快速開始
環境設定：確保您的 MSPM0 SDK 版本為 2.09.00.01 或更高。

配置 LED：在 empty_mspm0g1107.c 中修改 LED_NUM 以匹配您的燈條長度。

編譯與燒錄：使用 CCS 進行編譯，並觀察 UART 輸出（Baudrate 通常為 115200）。

📝 版本更新紀錄
V0.0.1 (2026-05-02):

完成 21 種特效集成與優化。

實作同一行更新的動態進度條系統。

導入 SECONDS() 宏時間管理機制。

🤝 關於作者
jackyxue