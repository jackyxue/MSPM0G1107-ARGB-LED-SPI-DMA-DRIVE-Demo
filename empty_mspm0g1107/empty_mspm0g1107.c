 #include "ti_msp_dl_config.h"
#include <stdlib.h>
#include <string.h>  // 提供 memset, memcpy 等函式
#include <stdio.h>
#include "debug.h" // 確保 uprint 在此定義

// --- 時間控制宏 ---
#define SYSTEM_FPS        25    // 根據您的 delay_cycles 估算，每秒約跑 25 幀
//#define SECONDS(s)        ((uint16_t)((s) * SYSTEM_FPS))

#define TICKS_PER_SECOND    50  // 每秒約 50 幀 (對應 delay 1.2M)
#define SECONDS(s)          ((uint16_t)(s * TICKS_PER_SECOND))

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

#define SATURATE_ADD(a, b) ((a + b > 255) ? 255 : a + b)

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

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;

// --- 2. 資料結構與全域變數 ---
typedef struct { uint8_t r; uint8_t g; uint8_t b; } RGB_Value;
typedef struct { char* name; uint16_t limit; void (*func)(uint16_t); } Effect_Type;
extern const Effect_Type EFFECT_TABLE[];

RGB_Value g_led_shadow[LED_NUM*2];            
uint8_t g_dma_spi_buffer[TOTAL_DMA_SIZE];   
volatile uint32_t g_system_ticks = 0;
uint16_t g_frame_counter = 0;
uint8_t g_current_mode = 0;
uint16_t g_brightness_int = 150; 

// 狀態機專用變數 (用於攤平 Stacking 等複雜特效)
static int g_stack_last = -1;
static int g_stack_move = 0;

uint32_t simple_rand(void);

// --- 3. 底層驅動與輔助函數 ---

static uint32_t next_seed = 1;
uint32_t simple_rand(void) {
    next_seed = next_seed * 1103515245 + 12345;
    return next_seed; // 直接傳回 uint32_t
}

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
    uint32_t idx = 0;
    for (int i = 0; i < LED_NUM; i++) {
        // 亮度縮放
        uint8_t r = (g_led_shadow[i].r * g_brightness_int) >> 8;
        uint8_t g = (g_led_shadow[i].g * g_brightness_int) >> 8;
        uint8_t b = (g_led_shadow[i].b * g_brightness_int) >> 8;

        // 編碼為 GRB 格式
        uint32_t color = (g << 16) | (r << 8) | b;
        for (int b_pos = 23; b_pos >= 0; b_pos--) {
            g_dma_spi_buffer[idx++] = (color & (1 << b_pos)) ? T1H : T0H;
        }
    }
    
    // 必須加入 Reset 訊號 (低電位維持 > 50us)
    memset(&g_dma_spi_buffer[idx], 0x00, RESET_BYTES);
    
    // 啟動 DMA 傳輸
    DL_DMA_setSrcAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) g_dma_spi_buffer);
    DL_DMA_setDestAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) (&SPI_0_INST->TXDATA));
    DL_DMA_setTransferSize(DMA, DMA_CH1_CHAN_ID, idx + RESET_BYTES);
    DL_DMA_enableChannel(DMA, DMA_CH1_CHAN_ID);
}

void fast_show_v0(void) {
    uint32_t idx = 0;
    for (int i = 0; i < LED_NUM; i++) {
        // 亮度縮放計算
        uint8_t r = (g_led_shadow[i].r * g_brightness_int) >> 8;
        uint8_t g = (g_led_shadow[i].g * g_brightness_int) >> 8;
        uint8_t b = (g_led_shadow[i].b * g_brightness_int) >> 8;

        // 將顏色編碼為符合 WS2811/WS2812 的 GRB 訊號
        uint32_t color = (g << 16) | (r << 8) | b;
        for (int b_pos = 23; b_pos >= 0; b_pos--) {
            g_dma_spi_buffer[idx++] = (color & (1 << b_pos)) ? T1H : T0H;
        }
    }
    
    // 加入 Reset 訊號 (低電位維持一段時間)
    memset(&g_dma_spi_buffer[idx], 0x00, RESET_BYTES);
    
    // 1. 設定 DMA 來源位址、目的位址與傳輸長度
    DL_DMA_setSrcAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) g_dma_spi_buffer);
    DL_DMA_setDestAddr(DMA, DMA_CH1_CHAN_ID, (uint32_t) (&SPI_0_INST->TXDATA));
    DL_DMA_setTransferSize(DMA, DMA_CH1_CHAN_ID, idx + RESET_BYTES);
    
    // 2. 使能 DMA 通道以等待 SPI 發出傳輸請求 (Trigger)
    DL_DMA_enableChannel(DMA, DMA_CH1_CHAN_ID);
}

void set_pixel(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < LED_NUM) { g_led_shadow[index].r = r; g_led_shadow[index].g = g; g_led_shadow[index].b = b; }
}

void set_all(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < LED_NUM; i++) set_pixel(i, r, g, b);
}

void clear_all() {
    memset(g_led_shadow, 0, sizeof(g_led_shadow));
}

void fade_all(uint8_t scale) {
    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (g_led_shadow[i].r * scale) >> 8;
        g_led_shadow[i].g = (g_led_shadow[i].g * scale) >> 8;
        g_led_shadow[i].b = (g_led_shadow[i].b * scale) >> 8;
    }
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

// 將 0-255 的數值轉換為 24-bit 的 RGB 顏色 (色相環)
uint32_t wheel(uint8_t pos) {
    pos = 255 - pos;
    if (pos < 85) return ((uint32_t)(255 - pos * 3) << 16) | (pos * 3);
    if (pos < 170) { pos -= 85; return ((uint32_t)(pos * 3) << 8) | (255 - pos * 3); }
    pos -= 170; return ((uint32_t)pos * 3 << 16) | ((uint32_t)(255 - pos * 3) << 8);
}

RGB_t wheel_to_rgb(uint8_t pos) {
    uint32_t color = wheel(pos); // 呼叫原本的 wheel
    RGB_t c;
    
    // 根據你原本 wheel 的排列順序進行位元偏移 (這裡以 RGB 為例)
    c.r = (uint8_t)(color >> 16);
    c.g = (uint8_t)(color >> 8);
    c.b = (uint8_t)(color);
    
    return c;
}

// 基礎工具：所有特效內部都用 set_led
void set_led(uint16_t n, uint8_t r, uint8_t g, uint8_t b) {
    if (n < LED_NUM) {
        g_led_shadow[n].r = r; g_led_shadow[n].g = g; g_led_shadow[n].b = b;
    }
}

// [修改重點]：Stacking 特效。原本的巢狀迴圈會阻塞，這裡改為「每一幀只動一步」
void eff_stacking_logic(uint16_t f) {
    if (g_stack_last == -1) g_stack_last = LED_NUM - 1;

    // A. 先清空上一幀
    memset(g_led_shadow, 0, sizeof(g_led_shadow));

    // B. 畫出底部已經堆好的
    for (int k = LED_NUM - 1; k > g_stack_last; k--) {
        set_led(k, 255, 100, 0); // 堆疊顏色
    }

    // C. 畫出移動中的那顆點
    set_led(g_stack_move, 255, 100, 0);

    // D. 更新邏輯位置 (取代原有的 for 迴圈)
    g_stack_move++;
    if (g_stack_move > g_stack_last) {
        g_stack_move = 0;
        g_stack_last--;
        if (g_stack_last < 0) g_stack_last = LED_NUM - 1; // 循環重置
    }
}

// [修改重點]：Pacifica。將計時器時間改為外部傳入的 f
void eff_pacifica_logic(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        uint16_t s1 = (uint16_t)(sin((i * 10 + f * 2) * 0.1) * 127 + 128);
        uint16_t s2 = (uint16_t)(sin((i * 5 - f * 4) * 0.1) * 127 + 128);
        set_led(i, (s1 * s2) >> 12, (s1 + s2) >> 3, (s1 + s2) >> 1);
    }
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

// 修正後的 Color Wipe：包含點亮與熄滅兩個階段
void effect_color_wipe_v2(uint16_t f) {
    uint16_t cycle_frames = LED_NUM * 2;
    uint16_t local_f = f % cycle_frames; 
    uint16_t step = local_f % LED_NUM;

    if (local_f < LED_NUM) {
        set_pixel(step, 0, 255, 0); 
    } 
    else {
        set_pixel(step, 0, 0, 0); 
    }
}

// [特效移植] Theater Chase (劇院跑馬燈) - 復古閃爍
void effect_theater_chase_v2(uint16_t f) {
    uint8_t q = (f / 4) % 3; 
    set_all(0, 0, 0); 
    for (int i = 0; i < LED_NUM; i += 3) {
        if ((i + q) < LED_NUM) {
            set_pixel(i + q, 20, 0, 20); 
        }
    }
}

// [特效移植] Rainbow Cycle (流暢彩虹)
void effect_rainbow_cycle_v2(uint16_t f) {
    uint8_t j = f & 255; 
    for (int i = 0; i < LED_NUM; i++) {
        uint32_t color = wheel(((i * 256 / LED_NUM) + j) & 255);
        set_pixel(i, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
}

// [特效移植] Bouncing Balls (彈跳小球)
void effect_bouncing_balls(uint16_t f) {
    static float ball_pos = 0;
    static float ball_vel = 0.5;

    if (f == 0) {
        ball_pos = 0;
        ball_vel = 0.5;
    }

    set_all(0, 0, 0);
    ball_pos += ball_vel;

    if (ball_pos >= (LED_NUM - 1) || ball_pos <= 0) {
        ball_vel = -ball_vel;
    }

    set_pixel((uint16_t)ball_pos, 150, 0, 255);
}

// [特效移植] Breathing (呼吸燈)
void effect_breathing(uint16_t f) {
    float intensity = (sin(f * 0.1) * 125) + 130; 
    g_brightness_int = (uint16_t)intensity;
    set_all(0, 100, 255); 
}

// [特效移植] Aurora (極光流體)
void effect_aurora(uint16_t f) {
    float off = f * 0.1; 
    for(int i = 0; i < LED_NUM; i++) {
        float n = (sin(i * 0.3 + off) + 1.0) / 2.0;
        uint8_t r = (uint8_t)(n * 50);
        uint8_t g = (uint8_t)(n * 255);
        uint8_t b = (uint8_t)((1.0 - n) * 200);
        set_pixel(i, r, g, b);
    }
}

// [特效移植] Rainbow Meteor (彩虹流星)
void effect_rainbow_meteor(uint16_t f) {
    uint16_t total_steps = LED_NUM + 20;
    uint16_t pos = f % total_steps;
    static uint8_t hue_offset = 0;
    if (pos == 0) hue_offset += 30;

    fade_canvas(180); 
    if (pos < LED_NUM) {
        uint32_t color = wheel(((pos * 256 / LED_NUM) + hue_offset) & 255);
        set_pixel(pos, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
}

// [特效移植] Random Twinkle (繁星點點)
void effect_twinkle(uint16_t f) {
    fade_canvas(220); 
    if (f % 2 == 0) {
        uint16_t p = rand() % LED_NUM; 
        set_pixel(p, 255, 255, 200); 
    }
}

// [特效移植] Dual Pulse (雙向擴散)
void effect_dual_pulse(uint16_t f) {
    uint16_t center = LED_NUM / 2;
    uint16_t i = f % (center + 5); 

    set_all(0, 0, 0);

    if (center + i < LED_NUM) set_pixel(center + i, 0, 150, 255); 
    if (center >= i)          set_pixel(center - i, 0, 150, 255);

    for (int j = 1; j < 4; j++) {
        uint8_t r_fade = 0;
        uint8_t g_fade = 150 / (j * 2);
        uint8_t b_fade = 255 / (j * 2);

        if (center + i >= j && (center + i - j) < LED_NUM) 
            set_pixel(center + i - j, r_fade, g_fade, b_fade);
        if ((center - i + j) < LED_NUM && (center >= (i - j))) 
            set_pixel(center - i + j, r_fade, g_fade, b_fade);
    }
}

// 特效 14: Fire2012 營火模擬
void effect_fire2012(uint16_t f) {
    static uint8_t heat[LED_NUM]; 
    uint8_t cooling = 50; 
    uint8_t sparking = 120;

    for (int i = 0; i < LED_NUM; i++) {
        uint8_t cooldown = (rand() % ((cooling * 10) / LED_NUM + 2));
        if (cooldown > heat[i]) heat[i] = 0;
        else heat[i] -= cooldown;
    }

    for (int k = LED_NUM - 1; k >= 2; k--) {
        heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
    }

    if ((rand() % 255) < sparking) {
        int y = rand() % (LED_NUM / 4 + 1); 
        heat[y] = heat[y] + (rand() % 95) + 160;
    }

    for (int j = 0; j < LED_NUM; j++) {
        uint8_t r = heat[j];
        uint8_t g = (heat[j] > 120) ? (heat[j] - 120) : 0; 
        uint8_t b = (heat[j] > 200) ? (heat[j] - 200) : 0; 
        set_pixel(j, r, g, b); 
    }
}

// 特效 15: TwinkleFOX 夢幻狐火
void effect_twinkle_fox(uint16_t f) {
    static uint8_t brightness_map[LED_NUM]; 
    static int8_t direction[LED_NUM]; 
    
    if (f == 0) {
        memset(brightness_map, 0, sizeof(brightness_map));
        memset(direction, 0, sizeof(direction));
    }

    if (rand() % 10 < 3) {
        int p = rand() % LED_NUM;
        if (brightness_map[p] == 0) {
            direction[p] = 1;
            brightness_map[p] = 5;
        }
    }

    for (int i = 0; i < LED_NUM; i++) {
        if (brightness_map[i] > 0) {
            brightness_map[i] += (direction[i] * 8); 
            if (brightness_map[i] >= 245) direction[i] = -1;
            if (brightness_map[i] <= 5) {
                brightness_map[i] = 0;
                direction[i] = 0;
            }
        }
        uint8_t br = brightness_map[i];
        set_pixel(i, br, br, (br >> 1)); 
    }
}

// 特效 16: Theater Chase
void effect_theater_chase_v3(uint16_t f) {
    uint8_t q = (f / 3) % 3; 
    set_all(0, 0, 0); 
    for (int i = 0; i < LED_NUM; i += 3) {
        if (i + q < LED_NUM) {
            set_pixel(i + q, 255, 100, 0); 
        }
    }
}

// 特效 17: Rainbow Theater Chase
void effect_rainbow_theater_chase(uint16_t f) {
    uint8_t q = (f / 3) % 3;
    uint16_t color_offset = (f * 2) & 255; 
    set_all(0, 0, 0);
    for (int i = 0; i < LED_NUM; i += 3) {
        if (i + q < LED_NUM) {
            uint32_t c = wheel((i + color_offset) & 255);
            set_pixel(i + q, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
    }
}

// 特效 18: 時尚幻彩流
void effect_fashion_flow(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        uint32_t c = wheel(((i * 256 / LED_NUM) + f) & 255);
        float wave = (sin(f * 0.05) + 1.2) / 2.2; 
        uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * wave);
        uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * wave);
        uint8_t b = (uint8_t)((c & 0xFF) * wave);
        set_pixel(i, r, g, b);
    }
}

// 特效 19: 雙向對沖流水
void effect_dual_scanner(uint16_t f) {
    uint16_t half_len = LED_NUM / 2;
    uint16_t i = f % (half_len + 5); 

    fade_canvas(160); 

    if (i < half_len) {
        set_pixel(i, 0, 255, 255);                
        set_pixel(LED_NUM - 1 - i, 0, 255, 255);  
    }
}

// 特效 20: 現代極光漸變
void effect_modern_aurora_v2(uint16_t f) {
    uint16_t j = f; 
    for (int i = 0; i < LED_NUM; i++) {
        uint32_t color = wheel(((i * 64 / LED_NUM) + j) & 255);
        set_pixel(i, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
}

// 特效: Confetti Pro (碎紙特效)
void eff_confetti_pro(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (g_led_shadow[i].r * 245) >> 8;
        g_led_shadow[i].g = (g_led_shadow[i].g * 245) >> 8;
        g_led_shadow[i].b = (g_led_shadow[i].b * 245) >> 8;
    }

    if (rand() % 100 < 15) {
        int pos = rand() % LED_NUM;
        uint32_t packed_color = wheel(rand() & 255);
        if(g_led_shadow[pos].r < 50 && g_led_shadow[pos].g < 50 && g_led_shadow[pos].b < 50) {
            g_led_shadow[pos].r = (uint8_t)(packed_color >> 16); 
            g_led_shadow[pos].g = (uint8_t)(packed_color >> 8);  
            g_led_shadow[pos].b = (uint8_t)(packed_color);       
        }
    }
}

// 特效: Stacking (堆疊)
void eff_stacking(uint16_t f) {
    if (g_stack_last == -1) g_stack_last = LED_NUM - 1;

    memset(g_led_shadow, 0, sizeof(g_led_shadow));

    for (int k = LED_NUM - 1; k > g_stack_last; k--) {
        g_led_shadow[k].r = 255; g_led_shadow[k].g = 150; g_led_shadow[k].b = 0;
    }

    g_led_shadow[g_stack_move].r = 255; 
    g_led_shadow[g_stack_move].g = 150; 
    g_led_shadow[g_stack_move].b = 0;

    g_stack_move++;
    if (g_stack_move > g_stack_last) {
        g_stack_move = 0;
        g_stack_last--;
        if (g_stack_last < 0) g_stack_last = LED_NUM - 1; 
    }
}

// 特效: Pacifica
void eff_pacifica(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        uint16_t s1 = (uint16_t)(sin((i * 10 + f * 2) * 0.1) * 127 + 128);
        uint16_t s2 = (uint16_t)(sin((i * 5 - f * 4) * 0.1) * 127 + 128);
        set_led(i, (s1 * s2) >> 12, (s1 + s2) >> 3, (s1 + s2) >> 1);
    }
}

// 特效: Pacman
void eff_pacman(uint16_t f) {
    static uint8_t pellets[300];
    int pac_pos = f % LED_NUM;
    if (pac_pos == 0) { 
        for(int i=0; i<LED_NUM; i++) pellets[i] = (i % 5 == 0);
    }
    clear_all();
    for(int i=0; i<LED_NUM; i++) if(pellets[i] && i > pac_pos) set_led(i, 20, 20, 20);
    set_led(pac_pos, 255, 200, 0); 
}

// 特效: Police
void eff_police(uint16_t f) {
    clear_all();
    if ((f / 2) % 2 == 0) { 
        for(int i=0; i<LED_NUM/2; i++) set_led(i, 5, 0, 0);
    } else {
        for(int i=LED_NUM/2; i<LED_NUM; i++) set_led(i, 0, 0, 5);
    }
}

// 特效: Beat Pulse
void eff_beat_pulse(uint16_t f) {
    float beat = f * 0.1;
    float br = (exp(sin(beat)) - 0.36) * 40.0;
    for(int i=0; i<LED_NUM; i++) set_led(i, (255 * (int)br) >> 8, 0, 0);
}

// WLED: Solid
void eff_solid(uint16_t f) {
    for(int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = 75;  
        g_led_shadow[i].g = 0;
        g_led_shadow[i].b = 130;
    }
}

// WLED: Color Wipe
void eff_color_wipe(uint16_t f) {
    uint16_t current_led = f % LED_NUM; 
    
    if (f == 0) memset(g_led_shadow, 0, sizeof(g_led_shadow));

    if (f < LED_NUM) {
        g_led_shadow[current_led].r = 0;
        g_led_shadow[current_led].g = 255;
        g_led_shadow[current_led].b = 0; 
    }
}

// WLED: Breathe
void eff_breathe(uint16_t f) {
    float angle = f * 0.05f;
    uint8_t br = (uint8_t)((sinf(angle) + 1.0f) * 125.0f) + 5;
    
    for(int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (0 * br) >> 8;   
        g_led_shadow[i].g = (0 * br) >> 8;
        g_led_shadow[i].b = (255 * br) >> 8;
    }
}

// WLED: Crossfade
void eff_crossfade(uint16_t f) {
    uint8_t step = (f * 255) / EFFECT_TABLE[g_current_mode].limit;
    
    RGB_Value colorA = {255, 0, 0}; 
    RGB_Value colorB = {0, 0, 255}; 

    for(int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = ( (colorA.r * (255 - step)) + (colorB.r * step) ) >> 8;
        g_led_shadow[i].g = ( (colorA.g * (255 - step)) + (colorB.g * step) ) >> 8;
        g_led_shadow[i].b = ( (colorA.b * (255 - step)) + (colorB.b * step) ) >> 8;
    }
}

// eff_37: 平滑流星 (改為完全依賴 g_led_shadow 且無 delay)
void eff_37_meteor_smooth(uint8_t r, uint8_t g, uint8_t b, uint8_t meteorSize, uint8_t fadeRate) {
    static uint32_t pos_fp = 0; 
    
    // 1. 全體淡出
    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (g_led_shadow[i].r <= fadeRate) ? 0 : g_led_shadow[i].r - (g_led_shadow[i].r * fadeRate >> 8);
        g_led_shadow[i].g = (g_led_shadow[i].g <= fadeRate) ? 0 : g_led_shadow[i].g - (g_led_shadow[i].g * fadeRate >> 8);
        g_led_shadow[i].b = (g_led_shadow[i].b <= fadeRate) ? 0 : g_led_shadow[i].b - (g_led_shadow[i].b * fadeRate >> 8);
    }

    // 2. 畫出流星頭部
    uint16_t ipos = pos_fp >> 8;
    for (int j = 0; j < meteorSize; j++) {
        int p = (int)ipos - j;
        if (p >= 0 && p < LED_NUM) {
            uint8_t br = 255 - (j * (255 / meteorSize));
            set_led(p, (r * br) >> 8, (g * br) >> 8, (b * br) >> 8);
        }
    }

    pos_fp += 128; 
    if ((pos_fp >> 8) >= LED_NUM + meteorSize) pos_fp = 0;
}

// eff_39: 彈跳球 (改為無 delay)
void eff_39_bouncing_balls(uint8_t ballCount) {
    #define MAX_BALLS 8
    static float pos[MAX_BALLS], v[MAX_BALLS];
    static float gravity = -0.03; 
    static uint8_t init = 0;
    
    if (!init) {
        for (int i = 0; i < MAX_BALLS; i++) {
            pos[i] = (float)(simple_rand() % 10);
            v[i] = (float)(simple_rand() % 15 + 15) / 10.0f;
        }
        init = 1;
    }

    clear_all(); 
    for (int i = 0; i < (ballCount > MAX_BALLS ? MAX_BALLS : ballCount); i++) {
        v[i] += gravity;
        pos[i] += v[i];

        if (pos[i] <= 0) {
            pos[i] = 0;
            v[i] = -v[i] * 0.95f; 
            if (v[i] < 1.8f) v[i] = (float)(simple_rand() % 15 + 20) / 10.0f;
        }
        if (pos[i] >= LED_NUM - 1) {
            pos[i] = LED_NUM - 1;
            v[i] = -v[i] * 0.8f;
        }

        int p = (int)pos[i];
        float frac = pos[i] - p;
        RGB_t c = wheel_to_rgb((i * 256 / ballCount) & 255);
        
        if (p >= 0 && p < LED_NUM) {
            uint8_t b1 = (uint8_t)(255 * (1.0f - frac));
            set_led(p, (c.r * b1) >> 8, (c.g * b1) >> 8, (c.b * b1) >> 8);
            if (p < LED_NUM - 1) {
                uint8_t b2 = (uint8_t)(255 * frac);
                set_led(p + 1, (c.r * b2) >> 8, (c.g * b2) >> 8, (c.b * b2) >> 8);
            }
        }
    }
}
 
// eff_40: 高性能彩虹追逐 (改為依賴 g_led_shadow 且無 delay)
void eff_40_chase_rainbow_fast(uint16_t speed) {
    static uint32_t rainbow_fp = 0;
    static uint32_t chaser_fp = 0;

    uint16_t off = (rainbow_fp >> 8) & 255;
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = wheel_to_rgb(((i * 256 / LED_NUM) + off) & 255);
        g_led_shadow[i].r = c.r;
        g_led_shadow[i].g = c.g;
        g_led_shadow[i].b = c.b;
    }

    for (int j = 0; j < 3; j++) {
        uint32_t p_fp = chaser_fp + (j * (LED_NUM << 8) / 3);
        p_fp %= (uint32_t)(LED_NUM << 8);
        
        uint16_t ip = p_fp >> 8;
        uint8_t frac = p_fp & 0xFF;
        int next_p = (ip + 1) % LED_NUM;

        uint8_t b1 = 255 - frac;
        uint8_t b2 = frac;

        g_led_shadow[ip].r = SATURATE_ADD(g_led_shadow[ip].r, b1);
        g_led_shadow[ip].g = SATURATE_ADD(g_led_shadow[ip].g, b1);
        g_led_shadow[ip].b = SATURATE_ADD(g_led_shadow[ip].b, b1);
        g_led_shadow[next_p].r = SATURATE_ADD(g_led_shadow[next_p].r, b2);
        g_led_shadow[next_p].g = SATURATE_ADD(g_led_shadow[next_p].g, b2);
        g_led_shadow[next_p].b = SATURATE_ADD(g_led_shadow[next_p].b, b2);
    }

    rainbow_fp += speed;
    chaser_fp += (speed * 3) >> 1; 
}

// eff_42: 專業劇院彩虹 (改為依賴 g_led_shadow 且無 delay)
void eff_42_theater_pro(uint8_t speed, uint8_t fade_rate) {
    static uint32_t theater_pos = 0;
    static uint8_t rainbow_j = 0;

    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (g_led_shadow[i].r * fade_rate) >> 8;
        g_led_shadow[i].g = (g_led_shadow[i].g * fade_rate) >> 8;
        g_led_shadow[i].b = (g_led_shadow[i].b * fade_rate) >> 8;
    }

    uint8_t offset = (theater_pos >> 8) % 3;
    for (int i = 0; i < LED_NUM; i += 3) {
        int pos = i + offset;
        if (pos < LED_NUM) {
            RGB_t c = wheel_to_rgb((i + rainbow_j) & 255);
            g_led_shadow[pos].r = SATURATE_ADD(g_led_shadow[pos].r, c.r);
            g_led_shadow[pos].g = SATURATE_ADD(g_led_shadow[pos].g, c.g);
            g_led_shadow[pos].b = SATURATE_ADD(g_led_shadow[pos].b, c.b);
        }
    }

    theater_pos += speed;
    rainbow_j++;
}
 
// eff_44: 對衝流星 (改為依賴 g_led_shadow 且無 delay)
void eff_44_multi_comet(uint16_t speed) {
    static uint32_t pos_fp = 0;
    
    for (int i = 0; i < LED_NUM; i++) {
        g_led_shadow[i].r = (g_led_shadow[i].r * 160) >> 8;
        g_led_shadow[i].g = (g_led_shadow[i].g * 160) >> 8;
        g_led_shadow[i].b = (g_led_shadow[i].b * 160) >> 8;
    }

    uint16_t p1 = (pos_fp >> 8) % LED_NUM;
    uint16_t p2 = (LED_NUM - 1) - p1;

    set_led(p1, 0, 150, 255); 
    set_led(p2, 255, 80, 0);  

    if (abs((int)p1 - (int)p2) < 2) {
        for(int i=0; i<LED_NUM; i++) {
            g_led_shadow[i].r = SATURATE_ADD(g_led_shadow[i].r, 120);
            g_led_shadow[i].g = SATURATE_ADD(g_led_shadow[i].g, 120);
            g_led_shadow[i].b = SATURATE_ADD(g_led_shadow[i].b, 120);
        }
    }

    pos_fp += speed;
}

// eff_43: 瘋狂頻閃 V2 (修正簽章，無 delay)
void eff_43_insane_strobe_v2(uint16_t f) {
    static uint8_t hue = 0;
    static uint8_t frame_cnt = 0;

    if (frame_cnt == 0) {
        RGB_t c = wheel_to_rgb(hue);
        for(int i=0; i<LED_NUM; i++) set_led(i, c.r, c.g, c.b);
        hue += 47; 
    } else {
        clear_all();
    }

    if (++frame_cnt >= 4) frame_cnt = 0;
}

// 定義 Wrapper 函數來傳遞參數給原始特效 (已移除 wait 參數)
void eff_37_wrapper(uint16_t f) {
    eff_37_meteor_smooth(255, 100, 0, 10, 60); 
}

void eff_39_wrapper(uint16_t f) {
    eff_39_bouncing_balls(3); 
}

void eff_40_wrapper(uint16_t f) {
    eff_40_chase_rainbow_fast(128); 
}

void eff_42_wrapper(uint16_t f) {
    eff_42_theater_pro(150, 200);
}

void eff_44_wrapper(uint16_t f) {
    eff_44_multi_comet(200);
}

void effect_auroral_flow(uint16_t f) {
    static float noise_off = 0.0;
    for (int i = 0; i < LED_NUM; i++) {
        float n = (sin(i * 0.1 + noise_off) + 1.0) / 2.0;
        set_pixel(i, n * 50, n * 255, (1.0 - n) * 200);
    }
    noise_off += 0.1;
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
    {"Red",      SECONDS(2), mode_static_rainbow},
    {"Orange",   SECONDS(2), mode_static_rainbow},
    {"Yellow",   SECONDS(2), mode_static_rainbow},
    {"Green",    SECONDS(2), mode_static_rainbow},
    {"Blue",     SECONDS(2), mode_static_rainbow},
    {"Indigo",   SECONDS(2), mode_static_rainbow},
    {"Purple",   SECONDS(2), mode_static_rainbow},
	
    {"Rainbow Static",  SECONDS(10), mode_static_rainbow},
    {"Meteor Smooth",   SECONDS(15), eff_37_wrapper},      
    {"Bouncing Balls",  SECONDS(20), eff_39_wrapper},      
    {"Rainbow Chase",   SECONDS(15), eff_40_wrapper},      
    {"Theater Pro",     SECONDS(15), eff_42_wrapper},      
    {"Multi Comet",     SECONDS(15), eff_44_wrapper},      
    {"Insane Strobe",   SECONDS(15), eff_43_insane_strobe_v2}, 
	
	{"Solid",    SECONDS(2),                eff_solid},      
    {"Wipe",     SECONDS(4),                eff_color_wipe}, 
    {"Breathe",  SECONDS(5),                eff_breathe},    
    {"Confetti", SECONDS(10),               eff_confetti_pro},
    {"Stacking", SECONDS(20),               eff_stacking},   
	
	{"Confetti", SECONDS(20), eff_confetti_pro},
    {"Stacking", SECONDS(30), eff_stacking},
    {"Pacifica", SECONDS(30), eff_pacifica},
    {"Pacman",   SECONDS(30), eff_pacman},
    {"Police",   SECONDS(15), eff_police},
    {"Pulse",    SECONDS(30), eff_beat_pulse},
	
	{"Aurora",  SECONDS(15), effect_auroral_flow},
    {"Meteor",  SECONDS(15), effect_rainbow_meteor},
    {"Fire",    SECONDS(20), effect_fire2012},
    {"Scanner", SECONDS(10), effect_dual_scanner},
	
	{"DualScanner",   SECONDS(12), effect_dual_scanner},
    {"ModernAurora",  SECONDS(20), effect_modern_aurora_v2},
	
	{"Fire2012",      SECONDS(20), effect_fire2012},
    {"TwinkleFox",    SECONDS(15), effect_twinkle_fox},
    {"TheaterChase",  SECONDS(10), effect_theater_chase_v3},
    {"RainbowTheater",SECONDS(15), effect_rainbow_theater_chase},
    {"FashionFlow",   SECONDS(15), effect_fashion_flow},
	
	{"Twinkle", SECONDS(10), effect_twinkle},
    {"Pulse",   SECONDS(12), effect_dual_pulse},
	{"Aurora",  SECONDS(15), effect_aurora},
    {"Meteor",  SECONDS(15), effect_rainbow_meteor},
	{"Breathing", SECONDS(15), effect_breathing},
	{"Bounce", SECONDS(30), effect_bouncing_balls},
	{"Rainbow", SECONDS(15), effect_rainbow_cycle_v2},
	{"Theater",  SECONDS(30), effect_theater_chase_v2},
	{"ColorWipe",SECONDS(30), effect_color_wipe_v2},

    {"Comet",    SECONDS(60), effect_comet},
    {"Rainbow",  SECONDS(60), effect_rainbow_cycle},
    {"Sparkle",  SECONDS(60), effect_sparkle},
    {"Scanner",  SECONDS(60), effect_scanner},
    {"Breath",   SECONDS(60), effect_breath},
    {"Theater",  SECONDS(60), wrap_theater},
    {"Cellular", SECONDS(120), effect_cellular_automata},
    {"Meteor",   SECONDS(60), effect_meteor},
    {"Fire",     SECONDS(60), effect_fire},
    {"Pulse",    SECONDS(60), effect_pulse},
    {"Crunch",   SECONDS(60), effect_big_crunch},
    {"Glitch",   SECONDS(60), effect_mandela_glitch},
    {"Download", SECONDS(30), effect_download_progress}
};

#define TOTAL_MODES (sizeof(EFFECT_TABLE) / sizeof(Effect_Type))

// --- 6. 主程式 ---
int main(void) {
    SYSCFG_DL_init();
    DL_SYSTICK_config(32000); 
    DL_SYSTICK_enableInterrupt();
    
    uprint("\033[2J\033[H=== ARGB PRO V0.0.1 ===\r\n");
    uint32_t mode_start_ms = get_ticks(); 

    while (1) {
        if (g_frame_counter % 5 == 0) {
            uint32_t elapsed_ms = get_ticks() - mode_start_ms;
            uint32_t progress = ((uint32_t)g_frame_counter * 100) / EFFECT_TABLE[g_current_mode].limit;
            
            uprint("\r\033[K >> [RUNNING] #%d %s | Time: %lu ms | Progress: %u%%", 
                   g_current_mode, 
                   EFFECT_TABLE[g_current_mode].name, 
                   (unsigned long)elapsed_ms, 
                   (unsigned int)progress);
        }

        if (g_current_mode != 11) g_brightness_int = 160; 

        EFFECT_TABLE[g_current_mode].func(g_frame_counter);
        
        // 統一在此處發送顯示，所有特效內部不可再呼叫 ws2812_show 或 delay
        fast_show(); 
        
        delay_cycles(1200000); 

        g_frame_counter++;
        
        if (g_frame_counter >= EFFECT_TABLE[g_current_mode].limit) {
            uint32_t final_ms = get_ticks() - mode_start_ms;

            uprint("\r\033[K[MODE] #%d: %-10s | Time: %lu ms | Progress: 100%% [DONE]\r\n", 
                   g_current_mode,
                   EFFECT_TABLE[g_current_mode].name, 
                   (unsigned long)final_ms);

            g_frame_counter = 0;
            g_current_mode = (g_current_mode + 1) % TOTAL_MODES;
            
            g_stack_last = -1;
            g_stack_move = 0;

            mode_start_ms = get_ticks();
        }
    }
}

int main_org(void) {
    SYSCFG_DL_init();
    DL_SYSTICK_config(32000); 
    DL_SYSTICK_enableInterrupt();
    uprint("\033[2J\033[H=== ARGB PRO V0.0.1 ===\r\n");
    uint32_t mode_start_ms = 0;

    while (1) {
        if (g_frame_counter == 0) {
            mode_start_ms = get_ticks();
        }

        if (g_frame_counter % 10 == 0) {
            uint32_t elapsed_ms = get_ticks() - mode_start_ms;
            uint32_t progress = ((uint32_t)g_frame_counter * 100) / EFFECT_TABLE[g_current_mode].limit;
            uprint("\r\033[K  >> Time<%s>: %lu ms | Progress: %u%%", EFFECT_TABLE[g_current_mode].name,(unsigned long)elapsed_ms, (unsigned int)progress);
        }

        if (g_current_mode != 11) g_brightness_int = 160;
        EFFECT_TABLE[g_current_mode].func(g_frame_counter);
        
        fast_show();
        
        delay_cycles(1200000);

        g_frame_counter++;
        
        if (g_frame_counter >= EFFECT_TABLE[g_current_mode].limit) {
            uint32_t final_ms = get_ticks() - mode_start_ms;

            uprint("\r\033[K[MODE] #%d: %-10s | Time: %lu ms | Progress: 100%% [DONE]\r\n", 
                   g_current_mode,
                   EFFECT_TABLE[g_current_mode].name, 
                   (unsigned long)final_ms);

            g_frame_counter = 0;
            g_current_mode = (g_current_mode + 1) % TOTAL_MODES;
            
            g_stack_last = -1;
            g_stack_move = 0;

            mode_start_ms = get_ticks();
        }
    }
}

void SysTick_Handler(void) { g_system_ticks++; }