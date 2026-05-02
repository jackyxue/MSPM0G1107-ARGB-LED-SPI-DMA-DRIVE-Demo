 #include "ti_msp_dl_config.h"
#include <stdlib.h>
#include <stdio.h>
#include "debug.h" // 確保 uprint 在此定義


// --- 時間控制宏 ---
#define SYSTEM_FPS        25    // 根據您的 delay_cycles 估算，每秒約跑 25 幀
#define SECONDS(s)        ((uint16_t)((s) * SYSTEM_FPS))

// --- 使用者定義展示時間 (單位：秒) ---
#define DEMO_TIME_DEFAULT 60
#define DEMO_TIME_SHORT   5
#define DEMO_TIME_LONG    120




// --- 1. 宏定義 ---
#define LED_NUM          120 
#define BITS_PER_LED     24
#define RESET_BYTES      100   
#define TOTAL_DMA_SIZE   ((LED_NUM * BITS_PER_LED) + RESET_BYTES)
#define T1H              0xFC   
#define T0H              0x80   

// 顏色索引定義 (Friendly for Engineers) ---
typedef enum {
    COLOR_IDX_RED = 0,
    COLOR_IDX_ORANGE,
    COLOR_IDX_YELLOW,
    COLOR_IDX_GREEN,
    COLOR_IDX_BLUE,
    COLOR_IDX_INDIGO,
    COLOR_IDX_PURPLE,
    COLOR_IDX_MAX
} LED_Color_Index;


// --- 2. 資料結構與全域變數 ---
typedef struct { uint8_t r; uint8_t g; uint8_t b; } RGB_Value;
typedef struct { char* name; uint16_t limit; void (*func)(uint16_t); } Effect_Type;

RGB_Value g_led_shadow[LED_NUM];            
uint8_t g_dma_spi_buffer[TOTAL_DMA_SIZE];   
volatile uint32_t g_system_ticks = 0;
uint16_t g_frame_counter = 0;
uint8_t g_current_mode = 0;
uint16_t g_brightness_int = 150; 

// --- 3. 底層驅動與輔助函數 ---
void encode_to_dma(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
    if (n >= LED_NUM) return;
    uint8_t ar = (uint16_t)(r * g_brightness_int) >> 8;
    uint8_t ag = (uint16_t)(g * g_brightness_int) >> 8;
    uint8_t ab = (uint16_t)(b * g_brightness_int) >> 8;
    uint8_t colors[3] = {ag, ar, ab}; 
    uint32_t base_idx = n * BITS_PER_LED;
    for (int c = 0; c < 3; c++) {
        for (int i = 7; i >= 0; i--) {
            g_dma_spi_buffer[base_idx + (c * 8) + (7 - i)] = (colors[c] & (1 << i)) ? T1H : T0H;
        }
    }
}

void fast_show(void) {
    for (int i = 0; i < LED_NUM; i++) {
        encode_to_dma(i, g_led_shadow[i].r, g_led_shadow[i].g, g_led_shadow[i].b);
    }
    while (DL_SPI_isBusy(SPI_0_INST));
    DL_DMA_disableChannel(DMA, DMA_CH1_CHAN_ID);
    for (int i = 0; i < RESET_BYTES; i++) {
        g_dma_spi_buffer[LED_NUM * BITS_PER_LED + i] = 0x00;
    }
    DL_DMA_setTransferSize(DMA, DMA_CH1_CHAN_ID, TOTAL_DMA_SIZE);
    DL_DMA_setSrcAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) &g_dma_spi_buffer[0]);
    DL_DMA_setDestAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) (&SPI_0_INST->TXDATA));
    DL_DMA_enableChannel(DMA, DMA_CH1_CHAN_ID);
}

void set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < LED_NUM) { g_led_shadow[index].r = r; g_led_shadow[index].g = g; g_led_shadow[index].b = b; }
}

void set_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_NUM; i++) set_pixel(i, r, g, b);
}

void fade_canvas(uint8_t scale) {
    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (uint16_t)(g_led_shadow[i].r * scale) >> 8;
        g_led_shadow[i].g = (uint16_t)(g_led_shadow[i].g * scale) >> 8;
        g_led_shadow[i].b = (uint16_t)(g_led_shadow[i].b * scale) >> 8;
    }
}

uint32_t get_wheel_color(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85) return ((uint32_t)(255 - pos * 3) << 16) | (pos * 3);
    if (pos < 170) { pos -= 85; return ((uint32_t)(pos * 3) << 8) | (255 - pos * 3); }
    pos -= 170; return ((uint32_t)pos * 3 << 16) | ((uint32_t)(255 - pos * 3) << 8);
}

uint32_t get_ticks(void) { return g_system_ticks; }

// --- 4. 特效實作 (所有的 void effect_... 必須放在這裡) ---

void mode_static_rainbow(uint16_t frame) {
    // 使用 g_current_mode 作為判斷基準
    switch(g_current_mode) {
        case COLOR_IDX_RED:    set_all(255, 0, 0);   break;
        case COLOR_IDX_ORANGE: set_all(160, 40, 0);  break;
        case COLOR_IDX_YELLOW: set_all(255, 255, 0); break;
        case COLOR_IDX_GREEN:  set_all(0, 255, 0);   break;
        case COLOR_IDX_BLUE:   set_all(0, 0, 255);   break;
        case COLOR_IDX_INDIGO: set_all(40, 20, 90);  break;
        case COLOR_IDX_PURPLE: set_all(20, 0, 45); break;
        default:               set_all(0, 0, 0);     break;
    }
}

void effect_comet(uint16_t frame) {
    fade_canvas(220);
    set_pixel(frame % LED_NUM, 0, 255, 255);
}

void effect_rainbow_cycle(uint16_t frame) {
    for (int i = 0; i < LED_NUM; i++) {
        uint32_t c = get_wheel_color(((i * 256 / LED_NUM) + frame) & 255);
        set_pixel(i, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF);
    }
}

void effect_sparkle(uint16_t frame) {
    fade_canvas(235);
    if (rand() % 30 > 27) set_pixel(rand() % LED_NUM, 50, 128, 200);
}

void effect_scanner(uint16_t frame) {
    fade_canvas(185);
    int pos = frame % (LED_NUM * 2);
    if (pos >= LED_NUM) pos = (LED_NUM * 2) - pos - 1;
    set_pixel(pos, 0, 255, 100);
}

void effect_breath(uint16_t frame) {
    int val = frame % 200;
    g_brightness_int = (val < 100) ? (val * 2) : (400 - val * 2);
    set_all(255, 50, 0);
}

void effect_theater_chase(uint16_t frame, uint8_t r, uint8_t g, uint8_t b) {
    set_all(0, 0, 0);
    for (int i = 0; i < LED_NUM; i += 3) set_pixel((i + (frame / 2)) % LED_NUM, r, g, b);
}

void effect_bouncing_ball(uint16_t frame) {
    fade_canvas(150);
    int range = (LED_NUM - 1) * 2;
    int pos = frame % range;
    if (pos >= LED_NUM) pos = range - pos;
    set_pixel(pos, 255, 0, 255);
}

void effect_meteor(uint16_t frame) {
    fade_canvas(200);
    static int meteor_pos = -1;
    if (frame == 0) meteor_pos = -1;
    if (meteor_pos < 0 && (rand() % 50 > 45)) meteor_pos = 0;
    if (meteor_pos >= 0) {
        set_pixel(meteor_pos, 0, 255, 128);
        meteor_pos++;
        if (meteor_pos >= LED_NUM) meteor_pos = -1;
    }
}

void effect_fire(uint16_t frame) {
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t flicker = rand() % 100;
        set_pixel(i, 255 - flicker, (uint16_t)((255 - flicker) * 40) >> 8, 0);
    }
}

void effect_pulse(uint16_t frame) {
    fade_canvas(100);
    int center = LED_NUM / 2;
    int offset = frame % (LED_NUM / 2);
    set_pixel(center + offset, 0, 150, 255);
    set_pixel(center - offset, 0, 150, 255);
}

void effect_big_crunch(uint16_t frame) {
    fade_canvas(180);
    int cycle = frame % 120;
    int offset = (int)((LED_NUM / 2) * (1.0f - ((float)cycle / 120.0f)));
    if (offset > 0) { set_pixel(LED_NUM/2+offset, 0, 50, 255); set_pixel(LED_NUM/2-offset, 0, 50, 255); }
    else set_all(255, 255, 255);
}

void effect_mandela_glitch(uint16_t frame) {
    static int glitch_timer = 0, offset_shift = 0;
    if (frame == 0) { glitch_timer = 0; offset_shift = 0; }
    for (int i = 0; i < LED_NUM; i++) {
        uint32_t c = get_wheel_color(((i * 256 / LED_NUM) + frame + offset_shift) % 256);
        set_pixel(i, (c>>16)&0xFF, (c>>8)&0xFF, c&0xFF);
    }
    if (glitch_timer == 0 && (rand() % 200 > 195)) { glitch_timer = rand()%10+5; offset_shift = rand()%LED_NUM; }
    if (glitch_timer > 0) { 
        for (int j = 0; j < LED_NUM; j += (rand()%5+1)) set_pixel(j, 255, 255, 255);
        glitch_timer--; 
    } else offset_shift = 0;
}

void effect_cellular_automata(uint16_t frame) {
    if (frame % 5 != 0) return;
    static uint8_t cells[120]; 
    uint8_t next_cells[120];
    if (frame == 0 || (frame % 500 == 0)) { 
        for(int i=0; i<LED_NUM; i++) cells[i] = 0;
        cells[rand() % LED_NUM] = 1; return; 
    }
    for (int i = 1; i < LED_NUM - 1; i++) next_cells[i] = cells[i-1] ^ (cells[i] | cells[i+1]);
    for (int i = 0; i < LED_NUM; i++) {
        cells[i] = next_cells[i];
        if (cells[i]) set_pixel(i, 255, 150, 0); else set_pixel(i, 20, 0, 40);
    }
}

void effect_download_progress(uint16_t frame) {
    uint8_t parts = (LED_NUM <= 60) ? 10 : (LED_NUM <= 120) ? 20 : 30;
    uint16_t current_led_limit = ((frame / 60) % (parts + 1)) * (LED_NUM / parts);
    set_all(0, 0, 0);
    for (int i = 0; i < current_led_limit && i < LED_NUM; i++) set_pixel(i, 0, 255, 0);
}

// --- 5. 表格封裝與定義 ---
void wrap_theater(uint16_t f) { effect_theater_chase(f, 255, 0, 0); }

const Effect_Type EFFECT_TABLE[] = {
    // 靜態模式：設定較短的時間，例如各 10 秒
    {"Red",      SECONDS(10), mode_static_rainbow},
    {"Orange",   SECONDS(10), mode_static_rainbow},
    {"Yellow",   SECONDS(10), mode_static_rainbow},
    {"Green",    SECONDS(10), mode_static_rainbow},
    {"Blue",     SECONDS(10), mode_static_rainbow},
    {"Indigo",   SECONDS(10), mode_static_rainbow},
    {"Purple",   SECONDS(10), mode_static_rainbow},

    // 動態特效：使用標準 60 秒
    {"Comet",    SECONDS(60), effect_comet},
    {"Rainbow",  SECONDS(60), effect_rainbow_cycle},
    {"Sparkle",  SECONDS(60), effect_sparkle},
    {"Scanner",  SECONDS(60), effect_scanner},
    {"Breath",   SECONDS(60), effect_breath},
    {"Theater",  SECONDS(60), wrap_theater},
    
    // 特殊演化特效：設定更長的時間
    {"Cellular", SECONDS(120), effect_cellular_automata},
    
    {"Meteor",   SECONDS(60), effect_meteor},
    {"Fire",     SECONDS(60), effect_fire},
    {"Pulse",    SECONDS(60), effect_pulse},
    {"Crunch",   SECONDS(60), effect_big_crunch},
    {"Glitch",   SECONDS(60), effect_mandela_glitch},
    {"Download", SECONDS(30), effect_download_progress}
};
const Effect_Type EFFECT_TABLExxx[] = {
    {"Red", 60, mode_static_rainbow}, 
	{"Orange", 60, mode_static_rainbow}, 
    {"Yellow", 60, mode_static_rainbow}, 
	{"Green", 60, mode_static_rainbow},
    {"Blue", 60, mode_static_rainbow}, 
	{"Indigo", 60, mode_static_rainbow},
    {"Purple", 60, mode_static_rainbow}, 
	{"Comet", 400, effect_comet},
    {"Rainbow", 400, effect_rainbow_cycle},
	{"Sparkle", 400, effect_sparkle},
    {"Scanner", 400, effect_scanner},
	{"Breath", 400, effect_breath},
    {"Theater", 400, wrap_theater},
	{"Bouncing", 400, effect_bouncing_ball},
    {"Meteor", 400, effect_meteor},
	{"Fire", 400, effect_fire},
    {"Pulse", 400, effect_pulse}, 
	{"Crunch", 400, effect_big_crunch},
    {"Glitch", 400, effect_mandela_glitch},
	{"Cellular", 1000, effect_cellular_automata},
    {"Download", 600, effect_download_progress}
};

#define TOTAL_MODES (sizeof(EFFECT_TABLE) / sizeof(Effect_Type))

// --- 6. 主程式 ---
int main(void) {
    SYSCFG_DL_init();
    DL_SYSTICK_config(32000); 
    DL_SYSTICK_enableInterrupt();
    uprint("\033[2J\033[H=== ARGB PRO V0.0.1 ===\r\n");
    uint32_t mode_start_ms = 0;

    while (1) {
        // --- 1. 模式啟動標題 ---
        if (g_frame_counter == 0) {
            mode_start_ms = get_ticks();
            // 在模式開始前換行並印出標題
            uprint("\r\n[MODE] #%d: %s\r\n", g_current_mode, EFFECT_TABLE[g_current_mode].name);
        }

        // --- 2. 進度更新 (同一行覆蓋) ---
        if (g_frame_counter % 10 == 0) {
            uint32_t elapsed_ms = get_ticks() - mode_start_ms;
            uint32_t progress = ((uint32_t)g_frame_counter * 100) / EFFECT_TABLE[g_current_mode].limit;
            
            // 這裡絕對不要加 \n，只用 \r 回到行首並用 \033[K 清除舊內容
            uprint("\r\033[K  >> Time: %lu ms | Progress: %u%%", (unsigned long)elapsed_ms, (unsigned int)progress);
        }

        // --- 3. 執行特效與顯示 (省略部分代碼) ---
        if (g_current_mode != 11) g_brightness_int = 160;
        EFFECT_TABLE[g_current_mode].func(g_frame_counter);
        fast_show();
        delay_cycles(1200000);

        // --- 4. 狀態更新與切換檢查 ---
        g_frame_counter++;
        
        if (g_frame_counter >= EFFECT_TABLE[g_current_mode].limit) {
            // 到達 100% 時，最後一次在同一行更新數據
            uint32_t final_ms = get_ticks() - mode_start_ms;
            uprint("\r\033[K  >> Time: %lu ms | Progress: 100%%", (unsigned long)final_ms);
            
            // 模式結束，在這裡才輸出換行符，讓下一個模式從新的一行開始
            uprint(" [DONE]\r\n"); 

            g_frame_counter = 0;
            g_current_mode = (g_current_mode + 1) % TOTAL_MODES;
        }
    }
}

void SysTick_Handler(void) { g_system_ticks++; }