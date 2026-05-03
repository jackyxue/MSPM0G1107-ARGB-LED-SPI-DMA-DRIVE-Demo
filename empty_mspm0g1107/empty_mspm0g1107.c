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
	COLOR_IDX_WHITE,
    COLOR_IDX_MAX
} LED_Color_Index;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGB_t;

#define RAIN_COUNT 8 // 同時存在的雨滴數量
#define POPCORN_COUNT 6 // 同時存在的爆米花數量

typedef struct {
    float pos;      // 位置
    float vel;      // 速度
    RGB_t color;    // 顏色
    bool active;    // 是否啟用
} Popcorn_t;

typedef struct {
    float pos;      // 位置
    float speed;    // 移動速度
    RGB_t color;    // 顏色
    bool active;    // 是否啟用
} Raindrop_t;

static Raindrop_t g_raindrops[RAIN_COUNT];
static Popcorn_t g_popcorns[POPCORN_COUNT];


RGB_t get_led(uint16_t index) ;
void set_led(uint16_t n, uint8_t r, uint8_t g, uint8_t b) ;

// --- 2. 資料結構與全域變數 ---
typedef struct { uint8_t r; uint8_t g; uint8_t b; } RGB_Value;
typedef struct { char* name; uint16_t limit; void (*func)(uint16_t); } Effect_Type;
extern const Effect_Type EFFECT_TABLE[];

static uint8_t  g_twinkle_steps[LED_NUM];    // 0: 背景, 1~127: 漸亮, 128~254: 漸暗
static uint8_t  g_twinkle_active_count = 0;  // 當前活躍的星星數量


static uint8_t g_hue = 0; // 全域色相循環

// 模擬 FastLED 的 fadeToBlackBy
void fade_to_black_by(uint8_t fade_factor) {
    float factor = (255 - fade_factor) / 255.0f;
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * factor), (uint8_t)(c.g * factor), (uint8_t)(c.b * factor));
    }
}


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

RGB_t get_led(uint16_t index) {
    RGB_t color = {0, 0, 0};

    if (index < LED_NUM) {
        // 根據您的定義，g_led_shadow 是 RGB_Value 陣列
        // 直接存取成員變數來解構顏色
        color.r = g_led_shadow[index].r;
        color.g = g_led_shadow[index].g;
        color.b = g_led_shadow[index].b;
    }

    return color;
}

uint32_t get_ticks(void) { return g_system_ticks; }

// --- 4. 特效實作 (所有的 void effect_... 必須放在這裡) ---

// 定義亮度：0.0 (全暗) 到 1.0 (全亮)
float g_brightness = 0.1f; 

void mode_static_rainbow(uint16_t frame) {
    uint8_t r = 0, g = 0, b = 0;

    switch(g_current_mode) {
        case COLOR_IDX_RED:    r = 255; g = 0;   b = 0;   break;
        case COLOR_IDX_ORANGE: r = 255; g = 60;  b = 0;   break; // 稍微調高原始值以利縮放
        case COLOR_IDX_YELLOW: r = 255; g = 255; b = 0;   break;
        case COLOR_IDX_GREEN:  r = 0;   g = 255; b = 0;   break;
        case COLOR_IDX_BLUE:   r = 0;   g = 0;   b = 255; break;
        case COLOR_IDX_INDIGO: r = 75;  g = 0;   b = 130; break;
        case COLOR_IDX_PURPLE: r = 128; g = 0;   b = 128; break;
        case COLOR_IDX_WHITE:  r = 255; g = 255; b = 255; break; 
        default:               r = 30;  g = 0;   b = 30;  break;
    }

    // 統一套用亮度縮放
    set_all((uint8_t)(r * g_brightness), 
            (uint8_t)(g * g_brightness), 
            (uint8_t)(b * g_brightness));
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


// 新特效：DNA 雙螺旋脈衝
void eff_dna_helix(uint16_t f) {
    // 1. 先背景微弱淡出，製造殘影感
    fade_canvas(180); 

    float speed = f * 0.15f; 
    
    for (int i = 0; i < LED_NUM; i++) {
        // 第一條螺旋 (科技藍)
        // 使用 $sin$ 函數計算亮度分佈
        float wave1 = sinf(i * 0.2f + speed);
        if (wave1 > 0.8f) { // 只點亮波峰部分
            set_led(i, 0, 100, 255); 
        }

        // 第二條螺旋 (生物紫) - 相位偏移 PI
        float wave2 = sinf(i * 0.2f + speed + 3.14159f);
        if (wave2 > 0.8f) {
            // 如果該位置已經被第一條螺旋點亮，則混色並增亮
            if (wave1 > 0.8f) {
                set_led(i, 200, 200, 255); // 交會點：亮白色
            } else {
                set_led(i, 150, 0, 250);   // 紫色
            }
        }
    }
}

void eff_singularity_pulse(uint16_t f) {
    // 1. 製造較重的殘影感，模擬能量流動
    fade_canvas(140); 

    uint16_t center = LED_NUM / 2;
    uint16_t cycle_len = 60; // 每個脈衝循環的長度
    uint16_t local_f = f % cycle_len;

    if (local_f < 30) {
        // A. 收縮階段：兩端向中間靠近
        uint16_t pos = (center * local_f) / 30;
        set_led(pos, 0, 255, 150);              // 左側能量
        set_led(LED_NUM - 1 - pos, 0, 255, 150); // 右側能量
    } 
    else if (local_f >= 30 && local_f < 35) {
        // B. 奇點階段：中心點爆發
        for (int i = center - 2; i <= center + 2; i++) {
            set_led(i, 255, 255, 255); // 純白閃爍
        }
    } 
    else {
        // C. 爆發階段：能量向外擴散並變色
        uint16_t dist = ((local_f - 35) * center) / 25;
        if (center + dist < LED_NUM) set_led(center + dist, 255, 50, 0);
        if (center >= dist) set_led(center - dist, 255, 50, 0);
    }
}

void eff_bio_heartbeat(uint16_t f) {
    static uint8_t r = 255, g = 0, b = 0;
    static uint16_t state_timer = 0;
    static uint8_t  step = 0;      // 0: 休息, 1: 第一跳, 2: 間隙, 3: 第二跳
    static uint16_t rest_frames = 50; 
    
    // 每個 frame 跑一次計時器
    state_timer++;

    float intensity = 0.0f;

    switch (step) {
        case 0: // 休息期 (全黑)
            if (state_timer > rest_frames) {
                // 休息結束，隨機選色並進入第一跳
                RGB_t c = wheel_to_rgb(simple_rand() & 255); 
                r = c.r; g = c.g; b = c.b;
                state_timer = 0;
                step = 1;
            }
            break;

        case 1: // 第一跳 (Lub - 強力爆發)
            // 使用指數函數產生極度非線性的亮度
            // 指數曲線: e^(-k * t^2)
            {
                float t = (float)state_timer / 10.0f; 
                intensity = expf(-5.0f * t * t); // 迅速衰減
                if (state_timer > 15) { state_timer = 0; step = 2; }
            }
            break;

        case 2: // 短暫間隙
            if (state_timer > 8) { state_timer = 0; step = 3; }
            break;

        case 3: // 第二跳 (Dub - 較弱的回聲)
            {
                float t = (float)state_timer / 12.0f;
                intensity = 0.4f * expf(-4.0f * t * t); 
                if (state_timer > 20) { 
                    state_timer = 0; 
                    step = 0; 
                    // 關鍵：隨機化下一次休息的時間長度 (20~80 frames)
                    rest_frames = 20 + (simple_rand() % 60); 
                }
            }
            break;
    }

    // 根據計算出的非線性亮度輸出
    for (int i = 0; i < LED_NUM; i++) { 
        set_led(i, (uint8_t)(r * intensity), (uint8_t)(g * intensity), (uint8_t)(b * intensity)); 
    }
}

void eff_cinematic_breath(uint16_t f) {
    // 1. 定義總週期 (例如 900 frames，約 15 秒完成一次「亮起+熄滅」的完整呼吸)
    const uint16_t TOTAL_FRAMES = 900; 
    uint16_t local_f = f % TOTAL_FRAMES;
    uint16_t cycle_count = f / TOTAL_FRAMES;

    static uint8_t r = 255, g = 255, b = 255;
    static uint16_t last_cycle = 0xFFFF;

    // 2. 只有在亮度最低點 (全新循環開始) 時才更換顏色
    if (cycle_count != last_cycle) {
        uint8_t random_hue = (uint8_t)(simple_rand() % 256); 
        RGB_t c = wheel_to_rgb(random_hue); 
        r = c.r; g = c.g; b = c.b;
        last_cycle = cycle_count;
    }

    // 3. 計算三角波進度 (t 從 0.0 -> 1.0 -> 0.0)
    float phase_progress = (float)local_f / (float)TOTAL_FRAMES;
    float triangle_t;

    if (phase_progress < 0.5f) {
        // 上升段：從 0.0 增加到 1.0
        triangle_t = phase_progress * 2.0f;
    } else {
        // 下降段：從 1.0 減少回 0.0
        triangle_t = (1.0f - phase_progress) * 2.0f;
    }

    // 4. 應用非線性曲線 (三次方或四次方)
    // 這會讓亮度在低點停留更久，營造「深呼吸」的感覺
    float intensity_factor = triangle_t * triangle_t * triangle_t; 
    
    // 5. 確保最低亮度為 1，最高為 255
    uint8_t brightness = 1 + (uint8_t)(254.0f * intensity_factor);

    // 6. 計算 RGB 分量並輸出給所有 WS2811 LED
    uint8_t out_r = (uint8_t)((r * brightness) / 255);
    uint8_t out_g = (uint8_t)((g * brightness) / 255);
    uint8_t out_b = (uint8_t)((b * brightness) / 255);

    for (int i = 0; i < LED_NUM; i++) { 
        set_led(i, out_r, out_g, out_b); 
    }
} 

void eff_cinematic_wave_breath(uint16_t f) {
    // 1. 定義總週期 (1800 frames，約 30 秒)
    const uint16_t TOTAL_FRAMES = 1800; 
    uint16_t local_f = f % TOTAL_FRAMES;
    uint16_t cycle_count = f / TOTAL_FRAMES;

    static uint8_t r = 255, g = 255, b = 255;
    static uint16_t last_cycle = 0xFFFF;

    // 2. 換色邏輯
    if (cycle_count != last_cycle) {
        uint8_t random_hue = (uint8_t)(simple_rand() & 255);
        RGB_t c = wheel_to_rgb(random_hue);
        r = c.r; g = c.g; b = c.b;
        last_cycle = cycle_count;
    }

    float phase = (float)local_f / (float)TOTAL_FRAMES;

    // 3. 邏輯分配：
    // 上升段: 0.0 ~ 0.4 (40%) -> 約 12 秒
    // 波浪段: 0.4 ~ 0.6 (20%) -> 約 6 秒 (符合你要求的 5~7 秒)
    // 下降段: 0.6 ~ 1.0 (40%) -> 約 12 秒

    if (phase >= 0.4f && phase < 0.6f) {
        // 【海洋區塊波浪段：精確控制在約 6 秒】
        float base_brightness = 15.0f; // 進一步降低背景亮度至 15
        float time_offset = (float)local_f * 0.06f; // 略微加快流動速度

        for (int i = 0; i < LED_NUM; i++) {
            // 產生多個循環移動的高亮區塊
            // i * 0.2f 調整區塊密度，powf(..., 30.0f) 讓區塊邊緣更銳利
            float wave = powf(0.5f + 0.5f * cosf(i * 0.2f - time_offset), 30.0f);
            
            float block_brightness = wave * 240.0f; 
            float final_b = base_brightness + block_brightness;
            if (final_b > 255.0f) final_b = 255.0f;

            set_led(i, (uint8_t)(r * final_b / 255), (uint8_t)(g * final_b / 255), (uint8_t)(b * final_b / 255));
        }
    } 
    else {
        // 【上升段與下降段】
        float intensity_factor;
        
        if (phase < 0.4f) {
            // 上升段 (0.0 -> 0.4)
            float t = phase / 0.4f;
            // 使用五次方曲線讓起步「極慢」，在亮度 40 以下停留更久
            intensity_factor = powf(t, 5.0f); 
        } else {
            // 下降段 (0.6 -> 1.0)
            float t = (1.0f - phase) / 0.4f;
            intensity_factor = t * t * t; 
        }

        uint8_t brightness = 1 + (uint8_t)(254.0f * intensity_factor); // 確保最低亮度 1
        uint8_t out_r = (uint8_t)((r * brightness) / 255);
        uint8_t out_g = (uint8_t)((g * brightness) / 255);
        uint8_t out_b = (uint8_t)((b * brightness) / 255);

        for (int i = 0; i < LED_NUM; i++) {
            set_led(i, out_r, out_g, out_b);
        }
    }
}

 
void eff_random_stack(uint16_t f) {
    // 既然 LED_NUM 是 #define 常數，這裡可以直接使用
    static uint8_t canvas_r[LED_NUM] = {0};
    static uint8_t canvas_g[LED_NUM] = {0};
    static uint8_t canvas_b[LED_NUM] = {0};
    
    static uint16_t stacked_total = 0;
    static float current_pos = 0.0f;
    static uint8_t current_r, current_g, current_b;
    static uint8_t current_size = 1;
    static bool need_new_block = true;

    // 1. 產生新的隨機塊
    if (need_new_block) {
        uint8_t hue = (uint8_t)(simple_rand() % 256);
        RGB_t c = wheel_to_rgb(hue);
        current_r = c.r; current_g = c.g; current_b = c.b;

        uint16_t remaining_space = LED_NUM - stacked_total;
        if (remaining_space == 0) {
            // 填滿後清空畫布重置
            for(int i = 0; i < LED_NUM; i++) { 
                canvas_r[i] = 0; canvas_g[i] = 0; canvas_b[i] = 0; 
            }
            stacked_total = 0;
            remaining_space = LED_NUM;
        }
        
        // 隨機選取 1~5 顆 LED 的長度
        current_size = 1 + (simple_rand() % 5);
        if (current_size > remaining_space) current_size = (uint8_t)remaining_space;

        current_pos = 0.0f;
        need_new_block = false;
    }

    // 2. 移動邏輯 (每次增加 2.5 顆 LED 的距離)
    current_pos += 2.5f; 

    // 3. 碰撞與固定邏輯
    float boundary = (float)(LED_NUM - stacked_total - current_size);
    if (current_pos >= boundary) {
        int start_idx = LED_NUM - stacked_total - current_size;
        for (int i = 0; i < current_size; i++) {
            int target = start_idx + i;
            if (target >= 0 && target < LED_NUM) {
                canvas_r[target] = current_r;
                canvas_g[target] = current_g;
                canvas_b[target] = current_b;
            }
        }
        stacked_total += current_size;
        need_new_block = true;
    }

    // 4. 渲染：結合移動塊與已固定的畫布
    for (int i = 0; i < LED_NUM; i++) {
        if (i >= (int)current_pos && i < (int)current_pos + current_size) {
            set_led(i, current_r, current_g, current_b);
        } else {
            set_led(i, canvas_r[i], canvas_g[i], canvas_b[i]);
        }
    }
} 


void eff_glitch_sparks(uint16_t f) {
    // 背景深色調 (深紫色)
    uint8_t bg_r = 10, bg_g = 0, bg_b = 20;

    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, bg_r, bg_g, bg_b);
    }

    // 隨機產生 2-3 個火花
    if ((simple_rand() % 10) > 7) { 
        int spark_pos = simple_rand() % LED_NUM;
        uint8_t spark_bright = 150 + (simple_rand() % 105);
        
        // 火花不僅是白色，帶有一點點隨機的青色偏移
        set_led(spark_pos, spark_bright - 50, spark_bright, spark_bright);
        
        // 增加火花的延展感 (鄰近顆粒)
        if (spark_pos > 0) set_led(spark_pos-1, 40, 60, 60);
        if (spark_pos < LED_NUM-1) set_led(spark_pos+1, 40, 60, 60);
    }
}


void effect_wled_twinkle(uint16_t f) {
    // 設定主色 (星星) 與 背景色
    RGB_t primary = {255, 255, 200}; // 暖白
    RGB_t secondary = {0, 5, 20};    // 深藍背景

    // 1. 每幀都有小機率產生新的星星
    // 調整 (simple_rand() % 100) < 5 中的 '5' 來改變星星產生的頻率
    if ((simple_rand() % 100) < 5) {
        uint16_t p = simple_rand() % LED_NUM;
        if (g_twinkle_steps[p] == 0) { // 只觸發目前處於背景狀態的 LED
            g_twinkle_steps[p] = 1; 
        }
    }

    // 2. 更新所有 LED 狀態並渲染
    for (int i = 0; i < LED_NUM; i++) {
        if (g_twinkle_steps[i] == 0) {
            // 處於背景狀態
            set_led(i, secondary.r, secondary.g, secondary.b);
        } else {
            // 處於閃爍週期中
            uint8_t ratio;
            if (g_twinkle_steps[i] < 128) {
                // 漸亮階段: 0 -> 255
                ratio = g_twinkle_steps[i] << 1; 
                g_twinkle_steps[i] += 4; // 調整此值改變漸變速度
            } else {
                // 漸暗階段: 255 -> 0
                ratio = 255 - ((g_twinkle_steps[i] - 128) << 1);
                if (g_twinkle_steps[i] > 250) {
                    g_twinkle_steps[i] = 0; // 結束週期，回到背景
                } else {
                    g_twinkle_steps[i] += 4;
                }
            }

            // 使用您現有的線性差值邏輯 (簡單版 Alpha Blend)
            uint8_t r = (uint8_t)(((uint16_t)primary.r * ratio + (uint16_t)secondary.r * (255 - ratio)) >> 8);
            uint8_t g = (uint8_t)(((uint16_t)primary.g * ratio + (uint16_t)secondary.g * (255 - ratio)) >> 8);
            uint8_t b = (uint8_t)(((uint16_t)primary.b * ratio + (uint16_t)secondary.b * (255 - ratio)) >> 8);
            set_led(i, r, g, b);
        }
    }
}

void effect_wled_tartan(uint16_t f) {
    // 定義 Tartan 的色板 (可根據喜好更換)
    RGB_t colors[] = {
        {200, 0, 0},   // 紅色 (主色)
        {0, 100, 0},   // 深綠 (次色)
        {255, 200, 0}, // 黃色 (細條紋)
        {0, 0, 0}      // 黑色 (間隔)
    };

    uint16_t speed = f * 2; // 調整移動速度

    for (int i = 0; i < LED_NUM; i++) {
        // 使用空間與時間的組合來決定當前位置的顏色索引
        // WLED Tartan 公式簡化版：藉由不同的除數產生粗細不一的條紋感
        uint16_t pos = (i + speed);
        
        RGB_t final_c;
        
        // 建立交錯感：利用模運算組合出格子特徵
        if ((pos % 32) < 12) {
            final_c = colors[0]; // 粗紅條
        } else if ((pos % 32) < 20) {
            final_c = colors[1]; // 中綠條
        } else if ((pos % 16) == 0) {
            final_c = colors[2]; // 黃色細線
        } else {
            final_c = colors[3]; // 背景
        }

        // 模擬「垂直」與「水平」帶狀交會的暗化效果 (Cross-hatch)
        // 這裡利用 i 本身與時間的反向偏移來疊加層次感
        if (((i - speed/2) % 24) < 4) {
            final_c.r = final_c.r >> 1;
            final_c.g = final_c.g >> 1;
            final_c.b = final_c.b >> 1;
        }

        set_led(i, final_c.r, final_c.g, final_c.b);
    }
}
 

void effect_wled_spots_fade(uint16_t f) {
    // 參數設定
    uint8_t spot_spacing = 8;    // 每隔 8 顆燈有一個點
    float speed = f * 0.1f;      // 縮放速度
    
    // 主色 (Spots) 與 背景色 (Solid background)
    RGB_t primary = {255, 100, 0}; // 橘色光點
    RGB_t bg = {20, 0, 40};        // 深紫色背景

    // 1. 先填入背景色
    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, bg.r, bg.g, bg.b);
    }

    // 2. 計算目前的縮放強度 (0.0 ~ 1.0)
    // 使用 sinf 產生呼吸感
    float intensity = (sinf(speed) + 1.0f) / 2.0f;

    // 3. 在固定間隔繪製光點
    for (int i = 0; i < LED_NUM; i += spot_spacing) {
        // 計算混合後的顏色
        uint8_t r = (uint8_t)(bg.r + (primary.r - bg.r) * intensity);
        uint8_t g = (uint8_t)(bg.g + (primary.g - bg.g) * intensity);
        uint8_t b = (uint8_t)(bg.b + (primary.b - bg.b) * intensity);
        
        set_led(i, r, g, b);

        // 如果要讓點「變大」，可以額外渲染鄰近的燈珠，但強度稍弱
        if (intensity > 0.7f) {
            float neighbor_int = (intensity - 0.7f) * 2.0f; // 鄰近燈珠的亮度
            uint8_t nr = (uint8_t)(bg.r + (primary.r - bg.r) * neighbor_int * 0.5f);
            uint8_t ng = (uint8_t)(bg.g + (primary.g - bg.g) * neighbor_int * 0.5f);
            uint8_t nb = (uint8_t)(bg.b + (primary.b - bg.b) * neighbor_int * 0.5f);
            
            if (i > 0) set_led(i - 1, nr, ng, nb);
            if (i < LED_NUM - 1) set_led(i + 1, nr, ng, nb);
        }
    }
}

 void effect_wled_popcorn2(uint16_t f) {
    float gravity = -0.15f; 
    
    // 1. 尾跡消散：使用 get_led 讀取後再進行衰減
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i); // 現在這裡會回傳正確的 RGB_t 型別
        
        // 緩慢變暗 (乘以 0.7 相當於保留 70% 亮度)
        set_led(i, (uint8_t)(c.r * 0.7f), 
                   (uint8_t)(c.g * 0.7f), 
                   (uint8_t)(c.b * 0.7f));
    }

    // 2. 粒子物理邏輯 (保持不變)
    for (int i = 0; i < POPCORN_COUNT; i++) {
        if (!g_popcorns[i].active) {
            if ((simple_rand() % 50) < 2) { 
                g_popcorns[i].pos = 0;
                g_popcorns[i].vel = 1.5f + (simple_rand() % 100) / 100.0f;
                // 隨機賦予鮮艷顏色
                g_popcorns[i].color.r = simple_rand() % 255;
                g_popcorns[i].color.g = simple_rand() % 255;
                g_popcorns[i].color.b = simple_rand() % 255;
                g_popcorns[i].active = true;
            }
            continue;
        }

        g_popcorns[i].pos += g_popcorns[i].vel;
        g_popcorns[i].vel += gravity;

        if (g_popcorns[i].pos < 0 || g_popcorns[i].pos >= LED_NUM) {
            g_popcorns[i].active = false;
            continue;
        }

        set_led((uint16_t)g_popcorns[i].pos, 
                g_popcorns[i].color.r, 
                g_popcorns[i].color.g, 
                g_popcorns[i].color.b);
    }
}


void effect_wled_sine(uint16_t f) {
    // 參數控制：頻率與移動速度
    float wave_frequency = 0.2f; // 波浪的密度
    float move_speed = f * 0.15f; // 波浪移動的速度
    
    // 定義基礎顏色 (您可以根據需要更改為隨時間變色)
    RGB_t base_color = {0, 150, 255}; // 水藍色

    for (int i = 0; i < LED_NUM; i++) {
        // 計算該位置的 Sine 值 (-1.0 到 1.0)
        // 公式：sin(位置 * 頻率 + 時間偏移)
        float s = sinf((i * wave_frequency) + move_speed);
        
        // 將 Sine 值轉換為亮度係數 (0.0 到 1.0)
        float intensity = (s + 1.0f) / 2.0f;

        // 計算最終顏色並寫入緩衝區
        uint8_t r = (uint8_t)(base_color.r * intensity);
        uint8_t g = (uint8_t)(base_color.g * intensity);
        uint8_t b = (uint8_t)(base_color.b * intensity);

        set_led(i, r, g, b);
    }
}


void effect_wled_dual_sine(uint16_t f) {
    float speed = f * 0.1f;
    
    for (int i = 0; i < LED_NUM; i++) {
        // 第一個波 (慢速，大波浪)
        float s1 = sinf((i * 0.1f) + speed);
        // 第二個波 (快速，細碎波浪)
        float s2 = sinf((i * 0.3f) - (speed * 1.5f));
        
        // 疊加並正規化
        float intensity = (s1 + s2 + 2.0f) / 4.0f;

        // 設定顏色 (例如紅藍交替感)
        set_led(i, 
                (uint8_t)(255 * intensity), 
                (uint8_t)(50 * (1.0f - intensity)), 
                (uint8_t)(150 * intensity));
    }
}

void effect_wled_sinelon(uint16_t f) {
    // 1. 全域消散 (Fade Out)，產生拖尾感
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.85f), (uint8_t)(c.g * 0.85f), (uint8_t)(c.b * 0.85f)); 
    }

    // 2. 計算位置：使用正弦波讓索引在 0 到 LED_NUM-1 之間平滑擺動
    // beatersin 邏輯：sinf 產生的範圍是 -1 到 1，將其縮放到燈條長度
    float pos_ratio = (sinf(f * 0.08f) + 1.0f) / 2.0f;
    uint16_t pos = (uint16_t)(pos_ratio * (LED_NUM - 1));

    // 3. 在當前位置繪製亮點
    set_led(pos, 0, 255, 255); // 預設青色眼球
}


void effect_wled_sinelon_dual(uint16_t f) {
    // 1. 消散拖尾[cite: 1]
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.8f), (uint8_t)(c.g * 0.8f), (uint8_t)(c.b * 0.8f)); 
    }

    // 2. 計算兩個方向的位置
    float sin_val = sinf(f * 0.06f);
    uint16_t pos1 = (uint16_t)(((sin_val + 1.0f) / 2.0f) * (LED_NUM - 1));
    uint16_t pos2 = (LED_NUM - 1) - pos1; // 反向位置

    // 3. 繪製兩個不同顏色的亮點 
    set_led(pos1, 255, 0, 100); // 桃紅色眼球[cite: 1]
    set_led(pos2, 0, 100, 255); // 藍色眼球[cite: 1]
}


void effect_wled_sinelon_rainbow(uint16_t f) {
    // 1. 消散拖尾[cite: 1]
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.9f), (uint8_t)(c.g * 0.9f), (uint8_t)(c.b * 0.9f)); 
    }

    // 2. 計算位置
    uint16_t pos = (uint16_t)(((sinf(f * 0.1f) + 1.0f) / 2.0f) * (LED_NUM - 1));

    // 3. 計算彩虹色 (利用時間 f 產生色相變化)
    // 這裡使用簡單的三角波模擬紅綠藍切換
    uint8_t r = (uint8_t)((sinf(f * 0.05f) + 1.0f) * 127);
    uint8_t g = (uint8_t)((sinf(f * 0.05f + 2.094f) + 1.0f) * 127); // 加上 120 度相位差
    uint8_t b = (uint8_t)((sinf(f * 0.05f + 4.188f) + 1.0f) * 127); // 加上 240 度相位差

    set_led(pos, r, g, b); 
}


void effect_wled_sweep(uint16_t f) {
    static uint16_t sweep_pos = 0;
    static bool filling_primary = true;
    static bool direction_forward = true;

    // 定義兩種切換的顏色
    RGB_t primary = {255, 0, 0};   // 紅色
    RGB_t secondary = {0, 0, 255}; // 藍色

    // 依照目前的進度決定填什麼顏色
    RGB_t color_to_set = filling_primary ? primary : secondary;
    set_led(sweep_pos, color_to_set.r, color_to_set.g, color_to_set.b); 

    // 更新位置
    if (direction_forward) {
        sweep_pos++;
        if (sweep_pos >= LED_NUM) {
            sweep_pos = LED_NUM - 1;
            direction_forward = false;
            filling_primary = !filling_primary; // 到達終點，切換顏色並折返
        }
    } else {
        if (sweep_pos > 0) {
            sweep_pos--;
        } else {
            direction_forward = true;
            filling_primary = !filling_primary; // 回到起點，切換顏色並再次前進
        }
    }
}

void effect_wled_sweep_random(uint16_t f) {
    static uint16_t sweep_pos = 0;
    static bool direction_forward = true;
    static RGB_t current_color = {255, 255, 255}; // 初始顏色

    // 繪製目前位置
    set_led(sweep_pos, current_color.r, current_color.g, current_color.b); 

    // 更新位置
    if (direction_forward) {
        sweep_pos++;
        if (sweep_pos >= LED_NUM) {
            sweep_pos = LED_NUM - 1;
            direction_forward = false;
            // 到達終點，隨機產生新顏色
            current_color.r = simple_rand() % 255;
            current_color.g = simple_rand() % 255;
            current_color.b = simple_rand() % 255;
        }
    } else {
        if (sweep_pos > 0) {
            sweep_pos--;
        } else {
            direction_forward = true;
            // 回到起點，再次隨機產生新顏色
            current_color.r = simple_rand() % 255;
            current_color.g = simple_rand() % 255;
            current_color.b = simple_rand() % 255;
        }
    }
}

void effect_wled_tv_simulator(uint16_t f) {
    static RGB_t cur_color = {0, 0, 0};
    static RGB_t target_color = {50, 100, 200}; // 初始目標色 (偏藍)
    static uint16_t hold_frames = 0;
    
    // 1. 如果 hold_frames 結束，切換到下一個隨機「場景」
    if (hold_frames == 0) {
        // 電視畫面通常偏藍、偏白或帶有些微暖色
        uint8_t base_bright = (simple_rand() % 150) + 50; 
        target_color.r = (uint8_t)(base_bright * ((simple_rand() % 100) / 150.0f));
        target_color.g = (uint8_t)(base_bright * ((simple_rand() % 100) / 120.0f));
        target_color.b = base_bright; // 藍色通常較強
        
        // 隨機決定這個場景持續多久 (30 到 150 幀之間)
        hold_frames = 30 + (simple_rand() % 120);
    } else {
        hold_frames--;
    }

    // 2. 平滑過度到目標顏色 (模擬螢幕淡入淡出)
    cur_color.r = (uint8_t)(cur_color.r * 0.9f + target_color.r * 0.1f);
    cur_color.g = (uint8_t)(cur_color.g * 0.9f + target_color.g * 0.1f);
    cur_color.b = (uint8_t)(cur_color.b * 0.9f + target_color.b * 0.1f);

    // 3. 渲染到整條燈帶
    // 有時候電視會有局部閃爍，我們加入微小的隨機擾動
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t noise = (simple_rand() % 10);
        set_led(i, 
            (cur_color.r > noise) ? cur_color.r - noise : 0, 
            (cur_color.g > noise) ? cur_color.g - noise : 0, 
            (cur_color.b > noise) ? cur_color.b - noise : 0
        ); 
    }
}

void effect_wled_running(uint16_t f) {
    // 參數控制
    float wave_length = 0.3f;  // 波長（數值越小波浪越長）
    float speed = f * 0.15f;    // 捲動速度
    
    // 定義主色與背景色
    RGB_t primary = {0, 255, 120}; // 薄荷綠
    RGB_t bg = {0, 20, 10};        // 深色背景

    for (int i = 0; i < LED_NUM; i++) {
        // 計算正弦波強度 (0.0 ~ 1.0)
        float s = sinf((i * wave_length) - speed);
        float intensity = (s + 1.0f) / 2.0f;

        // 線性插值混合顏色
        uint8_t r = (uint8_t)(bg.r + (primary.r - bg.r) * intensity);
        uint8_t g = (uint8_t)(bg.g + (primary.g - bg.g) * intensity);
        uint8_t b = (uint8_t)(bg.b + (primary.b - bg.b) * intensity);

        set_led(i, r, g, b); 
    }
}


void effect_wled_sine_waves_scrolling(uint16_t f) {
    float speed1 = f * 0.12f;
    float speed2 = f * 0.08f;
    
    for (int i = 0; i < LED_NUM; i++) {
        // 第一個波：向右捲動
        float w1 = sinf((i * 0.2f) - speed1);
        // 第二個波：向左捲動，頻率稍有不同
        float w2 = sinf((i * 0.15f) + speed2);
        
        // 疊加波形並縮放到 0.0 ~ 1.0
        float intensity = (w1 + w2 + 2.0f) / 4.0f;

        // 使用計算出的強度設定顏色 (例如紫色調)
        set_led(i, 
            (uint8_t)(100 * intensity), // R
            (uint8_t)(0),               // G
            (uint8_t)(255 * intensity)  // B
        ); 
    }
}

void effect_wled_rain(uint16_t f) {
    // 1. 背景消散：產生雨水滑過的拖尾殘影
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i); 
        // 快速消散 (保留 60%)，讓雨滴看起來更銳利
        set_led(i, (uint8_t)(c.r * 0.6f), (uint8_t)(c.g * 0.6f), (uint8_t)(c.b * 0.6f)); 
    }

    for (int i = 0; i < RAIN_COUNT; i++) {
        // 2. 隨機在「頂部」產生新的雨滴
        if (!g_raindrops[i].active) {
            if ((simple_rand() % 100) < 10) { // 降雨機率
                g_raindrops[i].pos = (float)(LED_NUM - 1); // 從燈條末端開始 (向下掉)
                g_raindrops[i].speed = 0.5f + (simple_rand() % 100) / 50.0f; // 隨機速度
                g_raindrops[i].color.r = 100; // 偏藍白色的雨
                g_raindrops[i].color.g = 150;
                g_raindrops[i].color.b = 255;
                g_raindrops[i].active = true;
            }
            continue;
        }

        // 3. 更新位置：往 0 的方向移動 (Notice Direction)
        g_raindrops[i].pos -= g_raindrops[i].speed; 

        // 4. 邊界檢查
        if (g_raindrops[i].pos < 0) {
            g_raindrops[i].active = false;
            continue;
        }

        // 5. 渲染當前雨滴亮點
        uint16_t idx = (uint16_t)g_raindrops[i].pos;
        if (idx < LED_NUM) {
            set_led(idx, g_raindrops[i].color.r, g_raindrops[i].color.g, g_raindrops[i].color.b); 
        }
    }
}

void effect_wled_random_color(uint16_t f) {
    static RGB_t current_c = {255, 255, 255};
    
    // 每 60 幀（約一秒，取決於您的 FPS）換一次顏色
    if (f % 60 == 0) {
        current_c.r = simple_rand() % 255;
        current_c.g = simple_rand() % 255;
        current_c.b = simple_rand() % 255;
    }

    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, current_c.r, current_c.g, current_c.b);
    }
}


void effect_wled_single_dynamic(uint16_t f) {
    // 第一次執行時先鋪滿隨機色
    if (f == 0) {
        for (int i = 0; i < LED_NUM; i++) {
            set_led(i, simple_rand() % 255, simple_rand() % 255, simple_rand() % 255);
        }
    }

    // 隨機挑選一個位置更新
    uint16_t target = simple_rand() % LED_NUM;
    set_led(target, simple_rand() % 255, simple_rand() % 255, simple_rand() % 255);
}

void effect_wled_multi_dynamic(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, simple_rand() % 255, simple_rand() % 255, simple_rand() % 255);
    }
}

void effect_wled_rainbow(uint16_t f) {
    // 使用相位差計算 RGB
    uint8_t r = (uint8_t)((sinf(f * 0.05f) + 1.0f) * 127);
    uint8_t g = (uint8_t)((sinf(f * 0.05f + 2.094f) + 1.0f) * 127);
    uint8_t b = (uint8_t)((sinf(f * 0.05f + 4.188f) + 1.0f) * 127);

    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, r, g, b);
    }
}

void effect_wled_rainbow_cycle(uint16_t f) {
    float wheel_speed = f * 0.05f;
    float spread = 0.1f; // 彩虹展開的密度

    for (int i = 0; i < LED_NUM; i++) {
        float angle = (i * spread) + wheel_speed;
        
        uint8_t r = (uint8_t)((sinf(angle) + 1.0f) * 127);
        uint8_t g = (uint8_t)((sinf(angle + 2.094f) + 1.0f) * 127);
        uint8_t b = (uint8_t)((sinf(angle + 4.188f) + 1.0f) * 127);

        set_led(i, r, g, b);
    }
}

void effect_wled_fade(uint16_t f) {
    // 利用 sinf 產生 0.0 ~ 1.0 的呼吸曲線
    float intensity = (sinf(f * 0.05f) + 1.0f) / 2.0f;
    
    RGB_t base = {255, 100, 0}; // 暖橘色

    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, (uint8_t)(base.r * intensity), 
                   (uint8_t)(base.g * intensity), 
                   (uint8_t)(base.b * intensity));
    }
}

// 16. Theater Chase (單色)
void effect_wled_theater_chase(uint16_t f) {
    uint8_t spacing = f % 3; // 每 3 幀移動一格
    
    for (int i = 0; i < LED_NUM; i++) {
        if ((i + spacing) % 3 == 0) {
            set_led(i, 255, 255, 255); // 亮白光
        } else {
            set_led(i, 0, 0, 0); // 關閉
        }
    }
}

// 17. Theater Chase Rainbow (彩虹版)
void effect_wled_theater_chase_rainbow(uint16_t f) {
    uint8_t spacing = f % 3;
    
    for (int i = 0; i < LED_NUM; i++) {
        if ((i + spacing) % 3 == 0) {
            // 隨時間變化的彩虹色
            uint8_t r = (uint8_t)((sinf(f * 0.05f) + 1.0f) * 127);
            uint8_t g = (uint8_t)((sinf(f * 0.05f + 2.094f) + 1.0f) * 127);
            uint8_t b = (uint8_t)((sinf(f * 0.05f + 4.188f) + 1.0f) * 127);
            set_led(i, r, g, b);
        } else {
            set_led(i, 0, 0, 0);
        }
    }
}

void effect_wled_running_lights(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        // 使用 sinf 根據位置與時間計算亮度
        float s = sinf(i * 0.2f + f * 0.1f);
        uint8_t level = (uint8_t)((s + 1.0f) * 127);
        set_led(i, level, level, level);
    }
}

// 21. Twinkle Fade (單色漸弱) & 22. Twinkle Fade Random (隨機色漸弱)
void effect_wled_twinkle_fade_random(uint16_t f) {
    // 1. 全體消散 (利用 get_led 讀取後衰減)
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.85f), (uint8_t)(c.g * 0.85f), (uint8_t)(c.b * 0.85f));
    }

    // 2. 隨機點亮新像素
    if (simple_rand() % 10 < 3) { // 調整閃爍頻率
        uint16_t target = simple_rand() % LED_NUM;
        // 隨機色
        set_led(target, simple_rand() % 255, simple_rand() % 255, simple_rand() % 255);
    }
}

 
void effect_wled_circus_combustus(uint16_t f) {
    uint8_t spacing = f % 6; // 每 6 幀移動一個循環
    
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t pos = (i + spacing) % 6;
        if (pos == 0)      set_led(i, 255, 255, 255); // 白色
        else if (pos == 2) set_led(i, 255, 0, 0);   // 紅色
        else               set_led(i, 0, 0, 0);     // 黑色（關閉）
    }
}

void effect_wled_halloween(uint16_t f) {
    uint8_t spacing = f % 4;
    
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t pos = (i + spacing) % 4;
        if (pos == 0)      set_led(i, 255, 100, 0); // 橘色
        else if (pos == 2) set_led(i, 128, 0, 255); // 紫色
        else               set_led(i, 0, 0, 0);
    }
}

void effect_wled_bicolor_chase(uint16_t f) {
    uint16_t pos = f % LED_NUM;
    RGB_t bg = {20, 0, 0};      // 深紅背景
    RGB_t chase = {0, 255, 255}; // 青色跑動點

    for (int i = 0; i < LED_NUM; i++) {
        if (i == pos || i == (pos + 2) % LED_NUM) {
            set_led(i, chase.r, chase.g, chase.b);
        } else {
            set_led(i, bg.r, bg.g, bg.b);
        }
    }
}

void effect_wled_tricolor_chase(uint16_t f) {
    uint8_t spacing = f % 3;
    
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t pos = (i + spacing) % 3;
        if (pos == 0)      set_led(i, 255, 0, 0);   // 紅
        else if (pos == 1) set_led(i, 0, 255, 0);   // 綠
        else               set_led(i, 0, 0, 255);   // 藍
    }
}

void effect_wled_twinkle_fox(uint16_t f) {
    // 1. 全體緩慢消散 (維持平滑的淡出感)
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        if (c.r > 0 || c.g > 0 || c.b > 0) {
            set_led(i, (uint8_t)(c.r * 0.92f), (uint8_t)(c.g * 0.92f), (uint8_t)(c.b * 0.92f));
        }
    }

    // 2. 隨機點亮（淡入開始）
    if (simple_rand() % 100 < 15) { // 控制密度
        uint16_t i = simple_rand() % LED_NUM;
        // 隨機暖色調
        set_led(i, 200 + (simple_rand() % 55), 150 + (simple_rand() % 50), 50);
    }
}

// 26. Strobe
void effect_wled_strobe(uint16_t f) {
    if (f % 2 == 0) set_all(255, 255, 255); // 快速閃爍
    else            set_all(0, 0, 0);
}

// 27. Strobe Rainbow
void effect_wled_strobe_rainbow(uint16_t f) {
    if (f % 2 == 0) {
        uint8_t r = (uint8_t)((sinf(f * 0.05f) + 1.0f) * 127);
        uint8_t g = (uint8_t)((sinf(f * 0.05f + 2.094f) + 1.0f) * 127);
        uint8_t b = (uint8_t)((sinf(f * 0.05f + 4.188f) + 1.0f) * 127);
        set_all(r, g, b);
    } else {
        set_all(0, 0, 0);
    }
}

// 30. Chase White (彩色點在白色背景跑)
void effect_wled_chase_white(uint16_t f) {
    set_all(40, 40, 40); // 背景白 (低亮度)
    set_led(f % LED_NUM, 255, 0, 0); // 紅色跑動點
}

// 37. Chase Blackout (黑色點在彩色背景跑)
void effect_wled_chase_blackout(uint16_t f) {
    set_all(0, 0, 255); // 背景藍
    set_led(f % LED_NUM, 0, 0, 0); // 黑色點
}


// 41. Running Red Blue (紅藍交替跑)
void effect_wled_running_red_blue(uint16_t f) {
    uint8_t spacing = f % 4;
    for (int i = 0; i < LED_NUM; i++) {
        if ((i + spacing) % 4 == 0)      set_led(i, 255, 0, 0); // 紅
        else if ((i + spacing) % 4 == 2) set_led(i, 0, 0, 255); // 藍
        else                             set_led(i, 0, 0, 0);
    }
}

// 47. Merry Christmas (綠紅交替跑)
void effect_wled_merry_christmas(uint16_t f) {
    uint8_t spacing = f % 6;
    for (int i = 0; i < LED_NUM; i++) {
        if ((i + spacing) % 6 == 0)      set_led(i, 0, 255, 0); // 綠
        else if ((i + spacing) % 6 == 3) set_led(i, 255, 0, 0); // 紅
        else                             set_led(i, 0, 0, 0);
    }
}


void effect_wled_larson_scanner(uint16_t f) {
    // 1. 全體快速消散
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.7f), 0, 0); // 僅保留紅色通道
    }

    // 2. 計算來回位置
    float pos_ratio = (sinf(f * 0.1f) + 1.0f) / 2.0f;
    uint16_t pos = (uint16_t)(pos_ratio * (LED_NUM - 1));

    // 3. 繪製亮點
    set_led(pos, 255, 0, 0);
}

void effect_wled_comet(uint16_t f) {
    // 1. 全體消散，產生長拖尾
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.92f), (uint8_t)(c.g * 0.92f), (uint8_t)(c.b * 0.92f));
    }

    // 2. 計算位置 (單向循環)
    uint16_t pos = f % LED_NUM;

    // 3. 繪製彗星頭部 (亮白色)
    set_led(pos, 255, 255, 255);
}

void effect_wled_fireworks_random(uint16_t f) {
    // 1. 緩慢消散 (火花冷卻)
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i);
        set_led(i, (uint8_t)(c.r * 0.88f), (uint8_t)(c.g * 0.88f), (uint8_t)(c.b * 0.88f));
    }

    // 2. 隨機產生「爆炸」火花
    if (simple_rand() % 100 < 5) { 
        uint16_t pos = simple_rand() % LED_NUM;
        // 隨機顏色
        set_led(pos, simple_rand() % 255, simple_rand() % 255, simple_rand() % 255);
    }
}

// 48. Fire Flicker (標準：強風感)
void effect_wled_fire_flicker(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        // 產生隨機亮度擾動
        uint8_t flicker = simple_rand() % 100;
        if (flicker > 60) {
            // 基礎火紅色 (R 高, G 低, B 極低)
            set_led(i, 200 + (simple_rand() % 55), 50 + (simple_rand() % 30), 0);
        } else {
            // 較暗的餘燼感
            set_led(i, 100 + (simple_rand() % 50), 20 + (simple_rand() % 10), 0);
        }
    }
}

// 49. Fire Flicker Soft (柔和版)
void effect_wled_fire_flicker_soft(uint16_t f) {
    static uint8_t last_vals[120]; // 假設 LED_NUM 為 120
    
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t target = 100 + (simple_rand() % 100);
        // 透過平滑權重降低跳動感
        last_vals[i] = (uint8_t)(last_vals[i] * 0.8f + target * 0.2f);
        set_led(i, last_vals[i], (uint8_t)(last_vals[i] * 0.2f), 0);
    }
}

// 50. Fire Flicker Intense (強烈版：色彩範圍更廣)
void effect_wled_fire_flicker_intense(uint16_t f) {
    for (int i = 0; i < LED_NUM; i++) {
        uint8_t r = 150 + (simple_rand() % 105);
        uint8_t g = simple_rand() % 100; // 加入更多黃色/綠色成分模擬高溫
        uint8_t b = simple_rand() % 20;
        set_led(i, r, g, b);
    }
}

// 11. Rainbow - 全體同步彩虹循環
void effect_wled_rainbow2(uint16_t f) {
    // 隨時間變化的基礎色相
    float angle = f * 0.05f;
    uint8_t r = (uint8_t)((sinf(angle) + 1.0f) * 127);
    uint8_t g = (uint8_t)((sinf(angle + 2.094f) + 1.0f) * 127);
    uint8_t b = (uint8_t)((sinf(angle + 4.188f) + 1.0f) * 127);

    for (int i = 0; i < LED_NUM; i++) {
        set_led(i, r, g, b);
    }
}

// 12. Rainbow Cycle - 燈條展開彩虹捲動
void effect_wled_rainbow_cycle2(uint16_t f) {
    float wheel_speed = f * 0.05f;
    float spread = 0.1f; // 彩虹的展開密度

    for (int i = 0; i < LED_NUM; i++) {
        float angle = (i * spread) + wheel_speed;
        uint8_t r = (uint8_t)((sinf(angle) + 1.0f) * 127);
        uint8_t g = (uint8_t)((sinf(angle + 2.094f) + 1.0f) * 127);
        uint8_t b = (uint8_t)((sinf(angle + 4.188f) + 1.0f) * 127);
        set_led(i, r, g, b);
    }
}

// 基礎 Color Wipe 邏輯 (支援方向與顏色反轉)
void effect_wled_color_wipe_generic(uint16_t f, bool reverse, bool inverse, bool random_mode) {
    static uint16_t wipe_pos = 0;
    static bool filling = true;
    static RGB_t color_main = {255, 0, 0}; // 預設紅色
    
    // 如果是隨機模式，在每一輪開始時更換顏色
    if (random_mode && wipe_pos == 0 && filling) {
        color_main.r = simple_rand() % 255;
        color_main.g = simple_rand() % 255;
        color_main.b = simple_rand() % 255;
    }

    uint16_t current_idx;
    if (!reverse) {
        current_idx = wipe_pos; // 正向：0 -> LED_NUM
    } else {
        current_idx = (LED_NUM - 1) - wipe_pos; // 反向：LED_NUM -> 0
    }

    // 決定要填入主色還是背景色 (Inverse 邏輯)
    if (filling) {
        if (!inverse) set_led(current_idx, color_main.r, color_main.g, color_main.b);
        else          set_led(current_idx, 0, 0, 0); // Inverse: 填滿時變成熄滅
    } else {
        if (!inverse) set_led(current_idx, 0, 0, 0); // 熄滅階段
        else          set_led(current_idx, color_main.r, color_main.g, color_main.b); // Inverse: 熄滅時變成填滿
    }

    // 更新進度
    wipe_pos++;
    if (wipe_pos >= LED_NUM) {
        wipe_pos = 0;
        filling = !filling;
    }
}

// 實作各別變體
void effect_wled_color_wipe(uint16_t f)         { effect_wled_color_wipe_generic(f, false, false, false); }
void effect_wled_color_wipe_inv(uint16_t f)     { effect_wled_color_wipe_generic(f, false, true, false); }
void effect_wled_color_wipe_rev(uint16_t f)     { effect_wled_color_wipe_generic(f, true, false, false); }
void effect_wled_color_wipe_rev_inv(uint16_t f) { effect_wled_color_wipe_generic(f, true, true, false); }
void effect_wled_color_wipe_random(uint16_t f)  { effect_wled_color_wipe_generic(f, false, false, true); }


 

void effect_demo_confetti(uint16_t f) {
    // 1. 全體緩慢淡出 (模擬 fadeToBlackBy)
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i); 
        // 降低 10/255 的亮度 (約保留 96%)
        set_led(i, (uint8_t)(c.r * 0.96f), (uint8_t)(c.g * 0.96f), (uint8_t)(c.b * 0.96f)); 
    }

    // 2. 隨機點亮新的彩色斑點
    if (simple_rand() % 100 < 15) {
        int pos = simple_rand() % LED_NUM;
        // 基於 g_hue 產生一個隨機變化的彩色
        uint8_t h_off = g_hue + (simple_rand() % 64);
        float h_angle = h_off * 0.1f;
        
        set_led(pos, 
            (uint8_t)((sinf(h_angle) + 1.0f) * 127), 
            (uint8_t)((sinf(h_angle + 2.094f) + 1.0f) * 127), 
            (uint8_t)((sinf(h_angle + 4.188f) + 1.0f) * 127)
        ); 
    }
}


void effect_demo_juggle(uint16_t f) {
    fade_to_black_by(20); 
    uint8_t dot_hue = 0;
    
    for (int i = 0; i < 8; i++) {
        // 使用 beatsin 概念，頻率隨 i 增加
        float freq = (i + 7) * 0.05f;
        uint16_t pos = (uint16_t)(((sinf(f * freq) + 1.0f) / 2.0f) * (LED_NUM - 1));
        
        // 疊加顏色到現有像素
        RGB_t old = get_led(pos); 
        set_led(pos, old.r + 30, old.g + 10, old.b + 50); // 模擬 |= CHSV
        dot_hue += 32;
    }
}

void effect_demo_bpm(uint16_t f) {
    uint8_t beat = (uint8_t)((sinf(f * 0.1f) + 1.0f) * 127); // 模擬 beatsin8
    
    for (int i = 0; i < LED_NUM; i++) {
        // 利用 g_hue 和位置 i 產生調色盤效果
        uint8_t r = (uint8_t)((sinf(g_hue + i * 0.2f) + 1.0f) * beat / 2.0f);
        uint8_t g = (uint8_t)((sinf(g_hue + i * 0.1f + 2.094f) + 1.0f) * beat / 2.0f);
        uint8_t b = (uint8_t)((sinf(g_hue + i * 0.3f + 4.188f) + 1.0f) * beat / 2.0f);
        set_led(i, r, g, b); 
    }
}


void effect_demo_rainbow(uint16_t f) {
    // 透過 spread 參數決定彩虹在燈條上的展開密度
    float spread = 0.15f; 
    
    for (int i = 0; i < LED_NUM; i++) {
        // 使用全域 g_hue 作為基礎偏移，加上位置 i 產生的相位差
        float angle = (g_hue * 0.1f) + (i * spread);
        
        uint8_t r = (uint8_t)((sinf(angle) + 1.0f) * 127);
        uint8_t g = (uint8_t)((sinf(angle + 2.094f) + 1.0f) * 127);
        uint8_t b = (uint8_t)((sinf(angle + 4.188f) + 1.0f) * 127);
        
        set_led(i, r, g, b); 
    }
}

void add_glitter(uint8_t chance) {
    // 模擬 random8() < chanceOfGlitter
    if ((simple_rand() % 255) < chance) {
        uint16_t pos = simple_rand() % LED_NUM;
        // 將該點直接設為純白，模擬 leds[pos] += CRGB::White
        set_led(pos, 255, 255, 255); 
    }
}

void effect_demo_rainbow_with_glitter(uint16_t f) {
    // 先繪製背景彩虹
    effect_demo_rainbow(f);
    // 加入 80/255 機率的閃爍
    add_glitter(80);
}


void effect_my_blue_breath(uint16_t f) {
    // 利用 sinf 產生 0.0 ~ 1.0 的亮度變化
    float intensity = (sinf(f * 0.1f) + 1.0f) / 2.0f;
    set_all(0, 0, (uint8_t)(255 * intensity)); 
}

void effect_start_gate(uint16_t f) {
    clear_all(); // 先清空畫布
    
    // 計算中心點
    uint16_t center = LED_NUM / 2;
    // 隨時間變化的展開距離 (0 到 center)
    uint16_t dist = (f / 2) % center; 

    // 從中心向兩端展開
    if (center + dist < LED_NUM) {
        set_led(center + dist, 255, 255, 255); // 向右
    }
    if (center >= dist) {
        set_led(center - dist, 255, 255, 255); // 向左
    }
}

void effect_start_travel(uint16_t f) {
    // 1. 全體快速消散，產生殘影
    for (int i = 0; i < LED_NUM; i++) {
        RGB_t c = get_led(i); 
        set_led(i, (uint8_t)(c.r * 0.7f), (uint8_t)(c.g * 0.7f), (uint8_t)(c.b * 0.8f)); 
    }

    // 2. 隨機產生新的「星光」
    if (simple_rand() % 100 < 20) {
        uint16_t pos = simple_rand() % LED_NUM;
        set_led(pos, 200, 200, 255); // 帶點藍的白光
    }
}

void effect_snake(uint16_t f) {
    static int16_t snake_head = 0;
    uint8_t snake_len = 5; // 蛇的長度
    
    clear_all(); // 每一幀重新繪製[cite: 1]

    // 繪製蛇身[cite: 1]
    for (int i = 0; i < snake_len; i++) {
        int16_t pos = (snake_head - i + LED_NUM) % LED_NUM;
        // 蛇頭比較亮，身體逐漸變暗
        uint8_t bright = 255 - (i * 40);
        set_led(pos, 0, bright, 0); // 綠色的蛇[cite: 1]
    }

    // 移動蛇頭
    if (f % 2 == 0) { // 控制爬行速度
        snake_head = (snake_head + 1) % LED_NUM;
    }
}

void effect_snake_growing(uint16_t f) {
    static int16_t snake_head = 0;      // 蛇頭位置
    static uint16_t current_len = 3;    // 初始長度
    static uint16_t food_pos = 10;      // 第一個食物的位置
    static RGB_t snake_color = {0, 255, 0}; // 初始顏色
    
    // 1. 每幀開始先清空背景
    clear_all(); 

    // 2. 繪製食物 (白色閃爍感)
    set_led(food_pos, 255, 255, 255); 

    // 3. 檢查是否「吃到食物」
    if (snake_head == food_pos) {
        // 吃到後增加長度 (隨機 1~5)
        current_len += (simple_rand() % 5) + 1;
        
        // 隨機更換蛇的顏色
        snake_color.r = simple_rand() % 255;
        snake_color.g = simple_rand() % 255;
        snake_color.b = simple_rand() % 255;
        
        // 重新生成食物位置 (不可與蛇頭重疊)
        food_pos = simple_rand() % LED_NUM;

        // 如果長度超過燈條總數，重置回小蛇
        if (current_len >= LED_NUM) {
            current_len = 3;
            uprint("\r\n[SNAKE] Reset to default length!\r\n"); // 透過 PA10/PA11 輸出重置訊息
        }
    }

    // 4. 繪製蛇身
    for (int i = 0; i < current_len; i++) {
        // 處理環形座標
        int16_t pos = (snake_head - i + LED_NUM) % LED_NUM;
        
        // 越靠近尾巴越暗 (漸層效果)
        float brightness = 1.0f - ((float)i / current_len);
        set_led(pos, 
            (uint8_t)(snake_color.r * brightness), 
            (uint8_t)(snake_color.g * brightness), 
            (uint8_t)(snake_color.b * brightness)); 
    }

    // 5. 控制移動速度[cite: 1]
    if (f % 2 == 0) {
        snake_head = (snake_head + 1) % LED_NUM; 
    }
}


void effect_snake_pro(uint16_t f) {
    static int16_t snake_head = 0;      // 蛇頭位置
    static uint16_t current_len = 3;    // 當前長度 (初始 3~5)
    static uint16_t food_pos = 10;      // 食物位置
    static uint8_t  food_energy = 2;    // 食物含有的長度 (1~5)
    static RGB_t snake_color = {0, 255, 0}; 

    // 1. 每幀清空緩衝區
    clear_all(); 

    // 2. 繪製食物：亮度根據 food_energy 變化，能量越高越亮
    uint8_t food_br = 50 + (food_energy * 40); 
    set_led(food_pos, food_br, food_br, food_br); // 白色食物[cite: 1]

    // 3. 碰撞偵測 (吃食物)[cite: 1]
    if (snake_head == food_pos) {
        // 蛇長度增加該食物的能量值[cite: 1]
        current_len += food_energy;
        
        // 變換蛇的顏色 (隨機)[cite: 1]
        snake_color.r = simple_rand() % 255;
        snake_color.g = simple_rand() % 255;
        snake_color.b = simple_rand() % 255;

        // 重新生成下一個食物的能量 (1~5) 與位置[cite: 1]
        food_energy = (simple_rand() % 5) + 1;
        food_pos = simple_rand() % LED_NUM; 

        // 串口輸出狀態監控 (透過 PA10/PA11)[cite: 1]
        uprint("\r\n[SNAKE] Nom! Len: %d | Next Food Energy: %d\r\n", current_len, food_energy);

        // 如果長度達到上限，重置回初始長度 (隨機 3~5)[cite: 1]
        if (current_len >= LED_NUM) {
            current_len = (simple_rand() % 3) + 3; 
            uprint("[SNAKE] Level Cleared! Resetting...\r\n"); 
        }
    }

    // 4. 繪製蛇身[cite: 1]
    for (int i = 0; i < current_len; i++) {
        int16_t pos = (snake_head - i + LED_NUM) % LED_NUM;
        
        // 尾巴漸暗效果，增加動態感[cite: 1]
        float fade = 1.0f - ((float)i / current_len);
        set_led(pos, 
            (uint8_t)(snake_color.r * fade), 
            (uint8_t)(snake_color.g * fade), 
            (uint8_t)(snake_color.b * fade)); 
    }

    // 5. 移動速度控制[cite: 1]
    if (f % 2 == 0) {
        snake_head = (snake_head + 1) % LED_NUM; 
    }
}

void effect_snake_multi_food(uint16_t f) {
    static int16_t snake_head = 0;      // 蛇頭位置
    static uint16_t current_len = 3;    // 當前長度 (初始 3~5)
    static uint16_t food_pos = 10;      // 食物起始位置
    static uint8_t  food_energy = 2;    // 食物長度 (1~5)
    static RGB_t snake_color = {0, 255, 0}; 

    // 1. 每幀清空背景
    clear_all(); 

    // 2. 繪製食物：現在食物由 food_energy 數量的 LED 組成
    for (int i = 0; i < food_energy; i++) {
        // 使用白色，且亮度稍微呼吸閃爍增加辨識度
        uint8_t pulse = 150 + (uint8_t)(sinf(f * 0.2f) * 100); 
        set_led((food_pos + i) % LED_NUM, pulse, pulse, pulse); 
    }

    // 3. 碰撞偵測：只要蛇頭撞到食物的「任何一部分」就算吃到
    bool ate_food = false;
    for (int i = 0; i < food_energy; i++) {
        if (snake_head == (food_pos + i) % LED_NUM) {
            ate_food = true;
            break;
        }
    }

    if (ate_food) {
        current_len += food_energy; // 增加對應長度
        
        // 變換顏色[cite: 1]
        snake_color.r = simple_rand() % 255;
        snake_color.g = simple_rand() % 255;
        snake_color.b = simple_rand() % 255;

        // 重新生成食物
        food_energy = (simple_rand() % 5) + 1; // 下一個食物長度 1~5[cite: 1]
        food_pos = simple_rand() % LED_NUM; 

        uprint("\r\n[SNAKE] ATE BIG FOOD! Len: %d | New Food Size: %d\r\n", current_len, food_energy); 

        // 滿長重置[cite: 1]
        if (current_len >= LED_NUM) {
            current_len = (simple_rand() % 3) + 3; 
            uprint("[SNAKE] Reset!\r\n"); 
        }
    }

    // 4. 繪製蛇身[cite: 1]
    for (int i = 0; i < current_len; i++) {
        int16_t pos = (snake_head - i + LED_NUM) % LED_NUM;
        float fade = 1.0f - ((float)i / current_len);
        set_led(pos, 
            (uint8_t)(snake_color.r * fade), 
            (uint8_t)(snake_color.g * fade), 
            (uint8_t)(snake_color.b * fade)); 
    }

    // 5. 移動速度控制[cite: 1]
    if (f % 2 == 0) {
        snake_head = (snake_head + 1) % LED_NUM; 
    }
}

void effect_snake_frozen(uint16_t f) {
    static int16_t snake_head = 0;      
    static uint16_t current_len = 3;    
    static uint16_t food_pos = 10;      
    static uint8_t  food_energy = 2;    
    static RGB_t snake_color = {0, 255, 0}; 
    
    // 凍結狀態管理
    static bool is_frozen = false;
    static uint32_t freeze_start_time = 0;

    uint32_t current_ms = get_ticks(); // 取得目前的系統時間

    // --- 1. 凍結邏輯檢查 ---
    if (is_frozen) {
        // 檢查是否已經凍結超過 2000 毫秒 (2秒)
        if (current_ms - freeze_start_time >= 2000) {
            is_frozen = false;
            // 重新開始：重置長度 (3~5) 與隨機顏色
            current_len = (simple_rand() % 3) + 3;
            snake_color.r = simple_rand() % 255;
            snake_color.g = simple_rand() % 255;
            snake_color.b = simple_rand() % 255;
            uprint("\r\n[SNAKE] Thawed! Restarting...\r\n"); 
        } else {
            // 凍結中：不更新任何邏輯，直接返回讓燈條保持現狀
            return; 
        }
    }

    // --- 2. 正常邏輯：清空與繪製食物 ---
    clear_all(); 
    for (int i = 0; i < food_energy; i++) {
        uint8_t pulse = 150 + (uint8_t)(sinf(f * 0.2f) * 100); 
        set_led((food_pos + i) % LED_NUM, pulse, pulse, pulse); 
    }

    // --- 3. 碰撞與成長邏輯 ---[cite: 1]
    bool ate_food = false;
    for (int i = 0; i < food_energy; i++) {
        if (snake_head == (food_pos + i) % LED_NUM) {
            ate_food = true;
            break;
        }
    }

    if (ate_food) {
        current_len += food_energy;
        
        // 檢查是否觸發「凍結條件」：長度超過 1/3[cite: 1]
        if (current_len > (LED_NUM / 3)) {
            is_frozen = true;
            freeze_start_time = current_ms;
            // 讓蛇變藍色表示被凍結了[cite: 1]
            snake_color = (RGB_t){0, 100, 255}; 
            uprint("\r\n[SNAKE] TOO LONG! FROZEN FOR 2s...\r\n"); 
        } else {
            // 正常換色與生成食物[cite: 1]
            snake_color.r = simple_rand() % 255;
            snake_color.g = simple_rand() % 255;
            snake_color.b = simple_rand() % 255;
            food_energy = (simple_rand() % 5) + 1;
            food_pos = simple_rand() % LED_NUM; 
        }
    }

    // --- 4. 繪製蛇身 ---[cite: 1]
    for (int i = 0; i < current_len; i++) {
        int16_t pos = (snake_head - i + LED_NUM) % LED_NUM;
        float fade = 1.0f - ((float)i / current_len);
        set_led(pos, 
            (uint8_t)(snake_color.r * fade), 
            (uint8_t)(snake_color.g * fade), 
            (uint8_t)(snake_color.b * fade)); 
    }

    // --- 5. 移動控制 ---[cite: 1]
    if (f % 2 == 0) {
        snake_head = (snake_head + 1) % LED_NUM; 
    }
}

 
 void effect_line_rgb_split(uint16_t f) {
    static int16_t offset = 0;       
    static int8_t  direction = 1;    
    static uint32_t last_move_ms = 0; 
    static RGB_t segment_colors[LED_NUM / 4 + 1]; 
    static bool colors_initialized = false;

    // 1. 初始化隨機顏色
    if (!colors_initialized) {
        for (int i = 0; i < (LED_NUM / 4 + 1); i++) {
            segment_colors[i].r = simple_rand() % 255;
            segment_colors[i].g = simple_rand() % 255;
            segment_colors[i].b = simple_rand() % 255;
        }
        colors_initialized = true;
    }

    // 2. 移動速度控制：每 1000ms (1秒) 移動一次位移量 2
    // 這樣達成「2 秒移動 2 個 LED 位置」的要求
    uint32_t current_ms = get_ticks();
    if (current_ms - last_move_ms >= 3000) { 
        offset += (2 * direction); // 每次跳動 2 格
        last_move_ms = current_ms;

        // 3. 邊界檢查與方向反轉
        if (offset >= (2 * LED_NUM)) {
            direction = -1;
            uprint("\r\n[LINE RGB] Reached Max, Turning Back...\r\n");
        } else if (offset <= -(2 * LED_NUM)) {
            direction = 1;
            uprint("\r\n[LINE RGB] Reached Min, Going Forward...\r\n");
        }
    }

    // 4. 繪製邏輯 (2亮2暗)
    clear_all();
    for (int i = 0; i < LED_NUM; i++) {
        int16_t logical_pos = (i - offset);
        int16_t pattern_idx = logical_pos % 4;
        if (pattern_idx < 0) pattern_idx += 4; 

        if (pattern_idx < 2) {
            int color_idx = (i / 4) % (LED_NUM / 4 + 1);
            set_led(i, segment_colors[color_idx].r, 
                       segment_colors[color_idx].g, 
                       segment_colors[color_idx].b);
        }
    }
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
    {"White",  SECONDS(2), mode_static_rainbow},
	
	{"Line RGB Split", 1000, effect_line_rgb_split}, 
	{"Frozen Snake", 2000, effect_snake_frozen},
	{"Snake Pro", 1800, effect_snake_multi_food}, // 設定較長的 limit 讓它有時間長大[cite: 1]
	{"Snake Pro", 1500, effect_snake_pro}, // 設定較長的 limit 讓它有時間長大[cite: 1]
	{"Growing Snake", 1200, effect_snake_growing}, // 給予較長的 limit 以完成成長循環[cite: 1]
	
	{"Start Gate",    300, effect_start_gate},    // 索引 X[cite: 1]
    {"Start Travel",  400, effect_start_travel},  // 索引 X+1[cite: 1]
    {"Snake",         600, effect_snake},         // 索引 X+2[cite: 1]
	
	// 新增 DemoReel100 與自定義特效
	
	
    {"Rainbow",           600,  effect_demo_rainbow},              // 執行約 10-20 秒
    {"Rainbow Glitter",   600,  effect_demo_rainbow_with_glitter}, // 彩虹加閃爍
    {"Blue Breath",       300,  effect_my_blue_breath},            // 藍色呼吸燈
    
    // 如果有實作 confetti
    {"Confetti",          450,  effect_demo_confetti},             // 五彩碎紙
	
	{"Demo: Confetti", SECONDS(10), effect_demo_confetti},
    {"Demo: Juggle",   SECONDS(10), effect_demo_juggle},
    {"Demo: BPM",      SECONDS(10), effect_demo_bpm},
    {"Sinelon",        SECONDS(10), effect_wled_sinelon}, // 之前已實作過
    {"Rainbow Cycle",  SECONDS(10), effect_wled_rainbow_cycle}, // 之前已實作過
	
	
	{"Rainbow Sync",        SECONDS(30), effect_wled_rainbow2},
    {"Rainbow Cycle",       SECONDS(30), effect_wled_rainbow_cycle2},
    {"Color Wipe",          SECONDS(20), effect_wled_color_wipe},
    {"Color Wipe Inv",      SECONDS(20), effect_wled_color_wipe_inv},
    {"Color Wipe Rev",      SECONDS(20), effect_wled_color_wipe_rev},
    {"Color Wipe Rev Inv",  SECONDS(20), effect_wled_color_wipe_rev_inv},
    {"Color Wipe Random",   SECONDS(20), effect_wled_color_wipe_random},
	
	{"Comet",               SECONDS(20), effect_wled_comet},
    {"Fireworks Random",    SECONDS(20), effect_wled_fireworks_random},
    {"Fire Flicker",        SECONDS(20), effect_wled_fire_flicker},
    {"Fire Flicker Soft",   SECONDS(20), effect_wled_fire_flicker_soft},
    {"Fire Intense",        SECONDS(20), effect_wled_fire_flicker_intense},
	
	
	{"Strobe",             SECONDS(10), effect_wled_strobe},
    {"Chase White",        SECONDS(20), effect_wled_chase_white},
    {"Running Red Blue",   SECONDS(20), effect_wled_running_red_blue},
    {"Larson Scanner",     SECONDS(30), effect_wled_larson_scanner},
    {"Merry Christmas",    SECONDS(20), effect_wled_merry_christmas},
	
	{"Circus Combustus", SECONDS(20), effect_wled_circus_combustus},
    {"Halloween",        SECONDS(20), effect_wled_halloween},
    {"Bicolor Chase",    SECONDS(20), effect_wled_bicolor_chase},
    {"Tricolor Chase",   SECONDS(20), effect_wled_tricolor_chase},
    {"TwinkleFOX",       SECONDS(30), effect_wled_twinkle_fox},
	
	{"Fade",                SECONDS(20), effect_wled_fade},
    {"Theater Chase",       SECONDS(20), effect_wled_theater_chase},
    {"Theater Rainbow",     SECONDS(20), effect_wled_theater_chase_rainbow},
    {"Running Lights",      SECONDS(20), effect_wled_running_lights},
    {"Twinkle",             SECONDS(15), effect_wled_twinkle},
    {"Twinkle Fade Random", SECONDS(15), effect_wled_twinkle_fade_random},
	
	{"Random Color",    SECONDS(15), effect_wled_random_color},
    {"Single Dynamic",  SECONDS(15), effect_wled_single_dynamic},
    {"Multi Dynamic",   SECONDS(10), effect_wled_multi_dynamic},
    {"Rainbow Sync",    SECONDS(30), effect_wled_rainbow},
    {"Rainbow Cycle",   SECONDS(30), effect_wled_rainbow_cycle},
	
	{"Rain Drops", SECONDS(30), effect_wled_rain},
	{"Running Scroll", SECONDS(30), effect_wled_running},
    {"Dual Sine Scroll", SECONDS(30), effect_wled_sine_waves_scrolling},
	{"TV Simulator", SECONDS(60), effect_wled_tv_simulator},
	{"Sweep Scan",    SECONDS(20), effect_wled_sweep},
    {"Sweep Random",  SECONDS(20), effect_wled_sweep_random},
	{"Sinelon",         SECONDS(20), effect_wled_sinelon},
    {"Sinelon Dual",    SECONDS(20), effect_wled_sinelon_dual},
    {"Sinelon Rainbow", SECONDS(20), effect_wled_sinelon_rainbow},
	
	{"Sine Wave", SECONDS(30), effect_wled_sine},
    {"Dual Sine", SECONDS(30), effect_wled_dual_sine},
	{"Popcorn Particles 2", SECONDS(30), effect_wled_popcorn2},
	{"Spots Fade", SECONDS(30), effect_wled_spots_fade},
	{"Tartan Kilt", SECONDS(30), effect_wled_tartan},
	{"Twinkle WLED",  SECONDS(30), effect_wled_twinkle},
	
	{"Glitch sparks", SECONDS(30), eff_glitch_sparks},
	{"Random Stack", SECONDS(60), eff_random_stack},
	{"Oceanic Heartbeat", SECONDS(60), eff_cinematic_wave_breath},
	{"Super-Slow Fade", SECONDS(30), eff_cinematic_breath},
	{"Heartbeat Bio-Pulse", SECONDS(30), eff_bio_heartbeat},
	{"Download", SECONDS(30), effect_download_progress},
	{"DNA Helix", SECONDS(15), eff_dna_helix},	
	{"Singularity Pulse", SECONDS(15), eff_singularity_pulse},
	
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
    {"Glitch",   SECONDS(60), effect_mandela_glitch}
   
};

#define TOTAL_MODES (sizeof(EFFECT_TABLE) / sizeof(Effect_Type))

// --- 6. 主程式 ---
int main(void) {
    SYSCFG_DL_init();
    DL_SYSTICK_config(32000); 
    DL_SYSTICK_enableInterrupt();
    
    uprint("\033[2J\033[H=== ARGB PRO V0.0.1 ===\r\n");
    
    uint32_t mode_start_ms = get_ticks(); 
    static uint32_t last_hue_update = 0; // 用於 DemoReel 特效的基礎色相計時

    while (1) {
        uint32_t current_ms = get_ticks(); // 取得目前的毫秒數

        // --- 新增：DemoReel100 色彩循環邏輯 ---
        // 每 20ms 自動增加色相偏移量，讓 Rainbow/Confetti 等特效能持續流動
        if (current_ms - last_hue_update > 20) {
            g_hue++; 
            last_hue_update = current_ms;
        }

        // --- 原有的進度顯示邏輯 ---
        if (g_frame_counter % 5 == 0) {
            uint32_t elapsed_ms = current_ms - mode_start_ms;
            uint32_t progress = ((uint32_t)g_frame_counter * 100) / EFFECT_TABLE[g_current_mode].limit;
            
            uprint("\r\033[K >> [RUNNING] #%d %s | Time: %lu ms | Progress: %u%%", 
                   g_current_mode, 
                   EFFECT_TABLE[g_current_mode].name, 
                   (unsigned long)elapsed_ms, 
                   (unsigned int)progress);
        }

        // --- 亮度管理 (這部分會根據不同特效手動修正) ---
        if (g_current_mode != 11) g_brightness_int = 160; 

        // 執行當前特效函式
        EFFECT_TABLE[g_current_mode].func(g_frame_counter);
        
        // 統一在此處發送顯示，所有特效內部不可再呼叫 ws2812_show 或 delay
        fast_show(); 
        
        // 幀率控制 (約 30-60 FPS)[cite: 1]
        delay_cycles(1200000); 

        g_frame_counter++;
        
        // --- 模式切換邏輯 ---
        if (g_frame_counter >= EFFECT_TABLE[g_current_mode].limit) {
            uint32_t final_ms = get_ticks() - mode_start_ms;

            uprint("\r\033[K[MODE] #%d: %-10s | Time: %lu ms | Progress: 100%% [DONE]\r\n", 
                   g_current_mode,
                   EFFECT_TABLE[g_current_mode].name, 
                   (unsigned long)final_ms);

            g_frame_counter = 0;
            g_current_mode = (g_current_mode + 1) % TOTAL_MODES;
            
            // 重置特效專用的內部狀態變數
            g_stack_last = -1;
            g_stack_move = 0;

            mode_start_ms = get_ticks();
        }
    }
}

int main_v101(void) {
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