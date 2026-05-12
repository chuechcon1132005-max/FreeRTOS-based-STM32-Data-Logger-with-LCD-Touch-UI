/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "LCD_Driver.h"
#include "bsp_driver_sd.h"
#include <stdbool.h>
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Định danh mới cho địa chỉ hiệu chuẩn để tránh xung đột với Driver của ST */
#define ADC_TS_CAL1       (*(volatile uint16_t*)0x1FFF7A2C) /* ADC tại 30°C */
#define ADC_TS_CAL2       (*(volatile uint16_t*)0x1FFF7A2E) /* ADC tại 110°C */
#define ADC_VREF_CAL      (*(volatile uint16_t*)0x1FFF7A2A) /* VREFINT tại 3.3V */

/* Dữ liệu hiệu chuẩn nhà máy STM32 — Nội suy tuyến tính 2 điểm chuẩn
   (30°C và 110°C) kết hợp VREFINT compensation → triệt tiêu offset chip. */

/* === Task Notification Flags cho TaskSD === */
/* Dùng osThreadFlagsSet/Wait thay vì Queue vì chỉ cần tín hiệu điều khiển,
   không cần truyền dữ liệu — nhẹ hơn và zero-copy. */
#define SD_FLAG_PLAY_START   0x01u  /* GUI yêu cầu bắt đầu ghi dữ liệu */
#define SD_FLAG_PLAY_STOP    0x02u  /* GUI yêu cầu dừng ghi */
#define SD_FLAG_READ_REQUEST 0x04u  /* GUI yêu cầu đọc file để hiển thị */
#define SD_FLAG_DELETE       0x08u  /* GUI yêu cầu xóa sạch file data.txt */
#define SD_FLAG_ALL          (SD_FLAG_PLAY_START | SD_FLAG_PLAY_STOP | SD_FLAG_READ_REQUEST | SD_FLAG_DELETE)

/* Macro kiểm tra flags hợp lệ: osThreadFlagsWait trả về mã lỗi âm
   (0xFFFFFFFx) khi timeout/error — các bit thấp trùng với SD_FLAG_*.
   Phải loại trừ để tránh kích hoạt giả (phantom trigger). */
#define SD_FLAGS_VALID(f)    (((f) & 0x80000000u) == 0u)

/* Chu kỳ ghi dữ liệu (ms) — đề bài yêu cầu mỗi 3 giây */
#define SD_LOG_INTERVAL_MS   3000u

/* Data Viewer: số dòng hiển thị trên 1 trang LCD */
#define SD_VIEWER_LINES_PER_PAGE  6u
/* Chiều dài tối đa 1 dòng dữ liệu trong file */
#define SD_VIEWER_LINE_MAX_LEN    56u
/* Kích thước buffer đọc file — đủ cho 1 trang */
#define SD_VIEWER_BUF_SIZE        (SD_VIEWER_LINES_PER_PAGE * SD_VIEWER_LINE_MAX_LEN)

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

RTC_HandleTypeDef hrtc;

SD_HandleTypeDef hsd;
DMA_HandleTypeDef hdma_sdio_rx;
DMA_HandleTypeDef hdma_sdio_tx;

SPI_HandleTypeDef hspi1;

PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* Definitions for TaskGUI */
osThreadId_t TaskGUIHandle;
const osThreadAttr_t TaskGUI_attributes = {
  .name = "TaskGUI",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for TaskSensor */
osThreadId_t TaskSensorHandle;
const osThreadAttr_t TaskSensor_attributes = {
  .name = "TaskSensor",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for TaskSD */
osThreadId_t TaskSDHandle;
const osThreadAttr_t TaskSD_attributes = {
  .name = "TaskSD",
  .stack_size = 1024 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for QueueData */
osMessageQueueId_t QueueDataHandle;
const osMessageQueueAttr_t QueueData_attributes = {
  .name = "QueueData"
};
/* Definitions for MutexSPI */
osMutexId_t MutexSPIHandle;
const osMutexAttr_t MutexSPI_attributes = {
  .name = "MutexSPI"
};
/* USER CODE BEGIN PV */

/* === Cấu trúc dữ liệu cho Data Viewer (Pause mode) === */
/* TaskSD đọc file vào buffer này, TaskGUI đọc buffer để vẽ LCD.
   Sử dụng cờ volatile 'ready' để đồng bộ — không cần mutex riêng
   vì chỉ có 1 writer (TaskSD) và 1 reader (TaskGUI). */
typedef struct {
  char     lines[SD_VIEWER_LINES_PER_PAGE][SD_VIEWER_LINE_MAX_LEN]; /* Các dòng text */
  uint16_t line_count;        /* Số dòng hợp lệ trong page hiện tại */
  uint16_t total_lines;       /* Tổng số dòng trong file */
  uint16_t current_page;      /* Trang hiện tại (0-based) */
  uint16_t total_pages;       /* Tổng số trang */
  volatile bool ready;        /* true = TaskSD đã đọc xong, GUI có thể vẽ */
  volatile bool request_page; /* true = GUI yêu cầu chuyển trang */
  uint16_t requested_page;    /* Số trang GUI muốn xem */
} sd_viewer_t;

static sd_viewer_t g_sd_viewer = {0};

/* Cờ báo TaskGUI cần vẽ lại toàn bộ giao diện chính sau khi thoát Data Viewer */
static volatile bool g_ui_need_full_redraw = false;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_RTC_Init(void);
static void MX_SDIO_SD_Init(void);
static void MX_SPI1_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
void StartTaskGUI(void *argument);
void StartTaskSensor(void *argument);
void StartTaskSD(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#define XPT_CMD_X  0xD0u
#define XPT_CMD_Y  0x90u
#define TOUCH_SPI_BR SPI_BAUDRATEPRESCALER_64

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

#define UI_BG             RGB565(255, 244, 235)
#define UI_BG_BOTTOM      RGB565(244, 232, 255)
#define UI_CARD           RGB565(255, 252, 246)
#define UI_CARD_SOFT      RGB565(244, 255, 246)
#define UI_CARD_EDGE      RGB565(214, 166, 142)
#define UI_CARD_EDGE_SOFT RGB565(228, 196, 178)
#define UI_ACCENT         RGB565(155, 96, 244)
#define UI_ACCENT_BRIGHT  RGB565(198, 143, 255)
#define UI_ACCENT_DIM     RGB565(193, 150, 239)
#define UI_TEXT_MAIN      RGB565(66, 44, 42)
#define UI_TEXT_SUB       RGB565(120, 84, 79)
#define UI_WARN           RGB565(255, 138, 76)
#define UI_OK             RGB565(63, 178, 106)
#define UI_ERR            RGB565(230, 82, 82)
#define UI_PLAY_ACTIVE_TOP RGB565(123, 220, 142)
#define UI_PLAY_ACTIVE_BOT RGB565(58, 176, 96)
#define UI_PAUSE_ACTIVE_TOP RGB565(255, 202, 128)
#define UI_PAUSE_ACTIVE_BOT RGB565(245, 144, 78)
#define UI_BUTTON_IDLE_TOP RGB565(255, 246, 232)
#define UI_BUTTON_IDLE_BOT RGB565(241, 226, 210)

#define BTN_PLAY_X       12u
#define BTN_PLAY_Y       162u
#define BTN_PAUSE_X      12u
#define BTN_PAUSE_Y      210u
#define BTN_W            108u
#define BTN_H            40u

#define TOUCH_RAW_X_MIN  210u
#define TOUCH_RAW_X_MAX  3860u
#define TOUCH_RAW_Y_MIN  260u
#define TOUCH_RAW_Y_MAX  3880u
#define TOUCH_SWAP_XY    0u
#define TOUCH_INVERT_X   1u
#define TOUCH_INVERT_Y   1u
#define TOUCH_STABLE_SPAN_MAX 180u
#define BTN_HIT_MARGIN_X 16u
#define BTN_HIT_MARGIN_Y 8u

/* WARNING: Enabling this will erase SD content when card has no FAT filesystem. */
#define SD_AUTO_FORMAT_ON_NOFS 1u
#define SD_MKFS_WORKBUF_SIZE   4096u
#define SD_NOFS_FORMAT_MAX_RETRY 3u
#define SD_POST_MKFS_SETTLE_MS 120u
#define SD_PREFER_1BIT_MODE    1u
#define SD_AUTOTUNE_ENABLE      1u
#define SD_AUTOTUNE_STEP_MS     2500u

#define TEMP_V25_MV      760

typedef enum {
  SD_STATE_CHECKING = 0,
  SD_STATE_READY,
  SD_STATE_MOUNTED,
  SD_STATE_ERROR
} sd_state_t;

typedef struct {
  uint16_t adc_raw_temp;
  uint16_t adc_raw_analog;
  int16_t temp_x10;
  bool play_enabled;
  bool pause_enabled;
  bool touch_pressed;
  uint16_t touch_x;
  uint16_t touch_y;
  sd_state_t sd_state;
  uint16_t sd_code;
} app_state_t;

static volatile app_state_t g_app = {
  .adc_raw_temp = 0u,
  .adc_raw_analog = 0u,
  .temp_x10 = 250,
  .play_enabled = false,
  .pause_enabled = false,
  .touch_pressed = false,
  .touch_x = 0xFFFFu,
  .touch_y = 0xFFFFu,
  .sd_state = SD_STATE_CHECKING,
  .sd_code = 0u
};

/* Buffer nhận: [0]=Internal Temp, [1]=VREFINT, [2]=IN10 (Analog Test Board) */
static volatile uint16_t g_adc_dma[3] = {0u, 0u, 0u};
static uint8_t g_touch_press_btn = 0u;
static uint8_t g_touch_last_hit = 0u;
static volatile uint32_t g_sd_last_hal_error = 0u;
static volatile HAL_StatusTypeDef g_sd_last_hal_status = HAL_OK;
static volatile uint8_t g_sd_init_stage = 0u;

#if SD_AUTOTUNE_ENABLE
static const uint8_t g_sd_tune_clock_divs[] = {10u, 8u, 6u, 4u, 2u};
#define SD_TUNE_PROFILE_COUNT ((uint8_t)(sizeof(g_sd_tune_clock_divs) / sizeof(g_sd_tune_clock_divs[0])))
#endif

static bool ui_in_rect(uint16_t x, uint16_t y, uint16_t rx, uint16_t ry, uint16_t rw, uint16_t rh)
{
  return (x >= rx) && (x < (rx + rw)) && (y >= ry) && (y < (ry + rh));
}

static uint8_t ui_hit_button_id(uint16_t x, uint16_t y)
{
  uint16_t play_rx = (BTN_PLAY_X > BTN_HIT_MARGIN_X) ? (BTN_PLAY_X - BTN_HIT_MARGIN_X) : 0u;
  uint16_t pause_rx = (BTN_PAUSE_X > BTN_HIT_MARGIN_X) ? (BTN_PAUSE_X - BTN_HIT_MARGIN_X) : 0u;
  uint16_t play_ry = (BTN_PLAY_Y > BTN_HIT_MARGIN_Y) ? (BTN_PLAY_Y - BTN_HIT_MARGIN_Y) : 0u;
  uint16_t pause_ry = (BTN_PAUSE_Y > BTN_HIT_MARGIN_Y) ? (BTN_PAUSE_Y - BTN_HIT_MARGIN_Y) : 0u;
  uint16_t rw = BTN_W + (BTN_HIT_MARGIN_X * 2u);
  uint16_t rh = BTN_H + (BTN_HIT_MARGIN_Y * 2u);
  bool play_hit;
  bool pause_hit;

  play_hit = ui_in_rect(x, y, play_rx, play_ry, rw, rh);
  pause_hit = ui_in_rect(x, y, pause_rx, pause_ry, rw, rh);

  if (play_hit && pause_hit) {
    uint16_t play_cy = BTN_PLAY_Y + (BTN_H / 2u);
    uint16_t pause_cy = BTN_PAUSE_Y + (BTN_H / 2u);
    uint16_t dy_play = (y > play_cy) ? (y - play_cy) : (play_cy - y);
    uint16_t dy_pause = (y > pause_cy) ? (y - pause_cy) : (pause_cy - y);
    return (dy_play <= dy_pause) ? 1u : 2u;
  }

  if (play_hit) {
    return 1u;
  }
  if (pause_hit) {
    return 2u;
  }
  return 0u;
}

static int16_t adc_temp_x10_from_raw(uint16_t raw_temp, uint16_t raw_vref)
{
  /* 1. Tính toán giá trị hiệu chuẩn từ bộ nhớ hệ thống */
  int32_t cal1 = (int32_t)ADC_TS_CAL1;
  int32_t cal2 = (int32_t)ADC_TS_CAL2;
  uint16_t vref_cal = ADC_VREF_CAL;
  
  if (cal2 <= cal1 || raw_vref == 0) return 250; 

  /* 2. Bù trừ sụt áp nguồn (Vrefint Compensation) */
  /* Chuẩn hóa giá trị ADC Temp về mức 3.3V của nhà máy */
  /* ADC_corrected = ADC_raw * VREFINT_CAL / VREFINT_DATA */
  int32_t raw_temp_corr = (int32_t)raw_temp * vref_cal / raw_vref;
  
  /* 3. Nội suy nhiệt độ từ 2 điểm hiệu chuẩn nhà máy (30°C và 110°C) */
  /* Công thức: T = 30 + 80 * (ADC_corrected - TS_CAL1) / (TS_CAL2 - TS_CAL1) */
  /* Kết quả x10 để giữ 1 chữ số thập phân: 300 = 30.0°C */
  int32_t temp_x10 = 300 + ((800 * (raw_temp_corr - cal1)) / (cal2 - cal1));
  
  return (int16_t)temp_x10;
}

static uint16_t ui_lerp565(uint16_t c0, uint16_t c1, uint16_t step, uint16_t max_step)
{
  uint16_t r0 = (uint16_t)((c0 >> 11) & 0x1Fu);
  uint16_t g0 = (uint16_t)((c0 >> 5) & 0x3Fu);
  uint16_t b0 = (uint16_t)(c0 & 0x1Fu);
  uint16_t r1 = (uint16_t)((c1 >> 11) & 0x1Fu);
  uint16_t g1 = (uint16_t)((c1 >> 5) & 0x3Fu);
  uint16_t b1 = (uint16_t)(c1 & 0x1Fu);
  uint16_t r;
  uint16_t g;
  uint16_t b;

  if (max_step == 0u) {
    return c0;
  }

  r = (uint16_t)((((uint32_t)r0 * (max_step - step)) + ((uint32_t)r1 * step)) / max_step);
  g = (uint16_t)((((uint32_t)g0 * (max_step - step)) + ((uint32_t)g1 * step)) / max_step);
  b = (uint16_t)((((uint32_t)b0 * (max_step - step)) + ((uint32_t)b1 * step)) / max_step);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void ui_fill_gradient_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t top, uint16_t bottom)
{
  uint16_t line;
  uint16_t den = (h > 1u) ? (h - 1u) : 1u;

  for (line = 0u; line < h; line++) {
    uint16_t c = ui_lerp565(top, bottom, line, den);
    lcd_draw_h_line(x, (uint16_t)(y + line), w, c);
  }
}

static void ui_draw_button(uint16_t x, uint16_t y, const uint8_t *label, bool active)
{
  bool is_play_btn = (y == BTN_PLAY_Y);
  uint16_t top;
  uint16_t bottom;
  uint16_t edge;
  uint16_t edge2;
  uint16_t text;

  if (active) {
    if (is_play_btn) {
      top = UI_PLAY_ACTIVE_TOP;
      bottom = UI_PLAY_ACTIVE_BOT;
      edge = RGB565(28, 125, 60);
      edge2 = RGB565(178, 245, 184);
    } else {
      top = UI_PAUSE_ACTIVE_TOP;
      bottom = UI_PAUSE_ACTIVE_BOT;
      edge = RGB565(184, 105, 42);
      edge2 = RGB565(255, 224, 183);
    }
    text = WHITE;
  } else {
    top = UI_BUTTON_IDLE_TOP;
    bottom = UI_BUTTON_IDLE_BOT;
    edge = UI_CARD_EDGE;
    edge2 = UI_CARD_EDGE_SOFT;
    text = UI_TEXT_MAIN;
  }

  ui_fill_gradient_rect(x, y, BTN_W, BTN_H, top, bottom);
  lcd_draw_rect(x, y, BTN_W, BTN_H, edge);
  lcd_draw_rect(x + 1u, y + 1u, BTN_W - 2u, BTN_H - 2u, edge2);
  lcd_draw_h_line(x + 4u, y + 6u, BTN_W - 8u, active ? RGB565(255, 255, 240) : RGB565(255, 252, 248));
  lcd_display_string(x + 26u, y + 13u, label, FONT_1206, text);
}

static void ui_draw_static(void)
{
  lcd_clear_screen(UI_BG);
  ui_fill_gradient_rect(0u, 0u, LCD_WIDTH, LCD_HEIGHT, UI_BG, UI_BG_BOTTOM);

  ui_fill_gradient_rect(0u, 0u, LCD_WIDTH, 38u, RGB565(255, 214, 168), RGB565(255, 170, 188));
  lcd_draw_h_line(0, 37, LCD_WIDTH, UI_CARD_EDGE);
  
  /* Header Branding: NHÓM 2 (Font lớn, Căn giữa hơn) */
  lcd_display_string(35, 10, (const uint8_t *)"NHOM 2", FONT_1608, UI_TEXT_MAIN);

  lcd_fill_rect(176, 8, 52, 20, RGB565(255, 244, 223));
  lcd_draw_rect(176, 8, 52, 20, RGB565(242, 145, 88));
  lcd_display_string(188, 14, (const uint8_t *)"LIVE", FONT_1206, RGB565(232, 112, 64));

  ui_fill_gradient_rect(12u, 46u, 216u, 106u, UI_CARD, RGB565(255, 240, 220));
  lcd_draw_rect(12, 46, 216, 106, RGB565(212, 162, 134));
  lcd_draw_rect(13, 47, 214, 104, RGB565(234, 197, 171));
  
  /* Nhãn tiêu đề được vẽ động trong ui_refresh_dynamic */
  /* Đường kẻ ngang phân tách tiêu đề và giá trị */
  lcd_draw_h_line(22, 74, 196, RGB565(231, 160, 130));
  /* Đường kẻ dọc phân tách 2 cột */
  lcd_draw_v_line(119, 48, 102, RGB565(231, 160, 130));
  /* Ô giá trị bên trái: Chip Temp */
  ui_fill_gradient_rect(16u, 78u, 100u, 70u, RGB565(255, 255, 247), RGB565(255, 244, 226));
  lcd_draw_rect(16, 78, 100, 70, RGB565(235, 206, 181));
  /* Ô giá trị bên phải: ADC Values */
  ui_fill_gradient_rect(122u, 78u, 102u, 70u, RGB565(244, 255, 247), RGB565(226, 255, 244));
  lcd_draw_rect(122, 78, 102, 70, RGB565(181, 220, 200));

  ui_draw_button(BTN_PLAY_X, BTN_PLAY_Y, (const uint8_t *)"PLAY", false);
  ui_draw_button(BTN_PAUSE_X, BTN_PAUSE_Y, (const uint8_t *)"PAUSE", false);

  ui_fill_gradient_rect(126u, 162u, 102u, 94u, RGB565(240, 255, 242), RGB565(229, 249, 255));
  lcd_draw_rect(126, 162, 102, 94, RGB565(128, 191, 165));
  lcd_draw_rect(127, 163, 100, 92, RGB565(173, 217, 196));
  lcd_display_string(134, 170, (const uint8_t *)"System", FONT_1206, UI_TEXT_SUB);
}

static void ui_draw_temp_value(int16_t temp_x10)
{
  uint16_t abs_v;
  uint16_t whole;
  uint16_t frac;
  uint16_t x = 26u;

  /* Xóa nền ô trái */
  ui_fill_gradient_rect(18u, 80u, 96u, 66u, RGB565(255, 255, 247), RGB565(255, 242, 220));

  if (temp_x10 < 0) {
    abs_v = (uint16_t)(-temp_x10);
    lcd_display_string(x, 96, (const uint8_t *)"-", FONT_1608, UI_WARN);
    x += 8u;
  } else {
    abs_v = (uint16_t)temp_x10;
  }

  whole = abs_v / 10u;
  frac = abs_v % 10u;

  lcd_display_num(x, 96, whole, 3, FONT_1608, UI_TEXT_MAIN);
  lcd_display_string((uint16_t)(x + 28u), 96, (const uint8_t *)".", FONT_1608, UI_TEXT_MAIN);
  lcd_display_num((uint16_t)(x + 36u), 96, frac, 1, FONT_1608, UI_ACCENT);
  lcd_display_string((uint16_t)(x + 48u), 96, (const uint8_t *)"C", FONT_1608, UI_TEXT_MAIN);
}

/**
 * @brief  Vẽ giá trị ADC của cả 2 kênh vào ô bên phải.
 *         Dòng 1: "T:" + ADC raw nhiệt độ chip nội
 *         Dòng 2: "A:" + ADC raw Analog Test Board
 */
static void ui_draw_adc_values(uint16_t adc_temp, uint16_t adc_analog)
{
  /* Xóa nền ô phải */
  ui_fill_gradient_rect(124u, 80u, 98u, 66u, RGB565(244, 255, 247), RGB565(230, 252, 242));

  /* Dòng 1: ADC nhiệt độ chip */
  lcd_display_string(128, 88, (const uint8_t *)"Chip:", FONT_1206, UI_TEXT_SUB);
  lcd_display_num(164, 88, adc_temp, 4, FONT_1206, UI_TEXT_MAIN);

  /* Dòng 2: ADC Analog Test Board */
  lcd_display_string(128, 108, (const uint8_t *)"Test:", FONT_1206, UI_TEXT_SUB);
  lcd_display_num(164, 108, adc_analog, 4, FONT_1206, UI_TEXT_MAIN);

  /* Đường phân cách nhỏ giữa 2 dòng */
  lcd_draw_h_line(130, 103, 86, RGB565(200, 230, 210));
}

static void ui_draw_status(bool play_enabled, bool pause_enabled, sd_state_t sd_state, uint16_t sd_code)
{
  ui_fill_gradient_rect(128u, 186u, 98u, 68u, RGB565(248, 255, 244), RGB565(236, 246, 255));
  lcd_draw_rect(128, 186, 98, 68, RGB565(172, 209, 181));

  lcd_display_string(130, 188, (const uint8_t *)"Play:", FONT_1206, UI_TEXT_SUB);
  lcd_display_string(168, 188, play_enabled ? (const uint8_t *)"ON" : (const uint8_t *)"OFF",
                     FONT_1206, play_enabled ? UI_OK : UI_TEXT_MAIN);

  lcd_display_string(130, 202, (const uint8_t *)"Pause:", FONT_1206, UI_TEXT_SUB);
  lcd_display_string(168, 202, pause_enabled ? (const uint8_t *)"ON" : (const uint8_t *)"OFF",
                     FONT_1206, pause_enabled ? UI_WARN : UI_TEXT_MAIN);

  lcd_display_string(130, 218, (const uint8_t *)"SD:", FONT_1206, UI_TEXT_SUB);
  switch (sd_state) {
  case SD_STATE_CHECKING:
    if (sd_code == 90u) {
      lcd_display_string(154, 218, (const uint8_t *)"FMT", FONT_1206, UI_WARN);
    } else {
      lcd_display_string(154, 218, (const uint8_t *)"CHK", FONT_1206, RGB565(122, 96, 212));
    }
    break;
  case SD_STATE_MOUNTED:
    lcd_display_string(154, 218, (const uint8_t *)"OK", FONT_1206, UI_OK);
    break;
  case SD_STATE_READY:
    if (sd_code == FR_NO_FILESYSTEM) {
      lcd_display_string(154, 218, (const uint8_t *)"NOFS", FONT_1206, UI_WARN);
    } else {
      lcd_display_string(154, 218, (const uint8_t *)"RDY", FONT_1206, RGB565(106, 90, 210));
    }
    break;
  case SD_STATE_ERROR:
    lcd_display_string(154, 218, (const uint8_t *)"ERR", FONT_1206, UI_ERR);
    lcd_display_num(176, 218, sd_code, 3, FONT_1206, UI_WARN);
    break;
  default:
    lcd_display_string(154, 218, (const uint8_t *)"CHK", FONT_1206, RGB565(122, 96, 212));
    break;
  }

  lcd_fill_rect(128, 232, 98, 16, RGB565(244, 252, 239));
  if (g_app.touch_pressed) {
    lcd_display_string(130, 234, (const uint8_t *)"T:", FONT_1206, UI_TEXT_SUB);
    lcd_display_num(144, 234, g_app.touch_x, 3, FONT_1206, UI_TEXT_MAIN);
    lcd_display_string(166, 234, (const uint8_t *)",", FONT_1206, UI_TEXT_SUB);
    lcd_display_num(172, 234, g_app.touch_y, 3, FONT_1206, UI_TEXT_MAIN);
  } else {
    lcd_display_string(130, 234, (const uint8_t *)"T:---,---", FONT_1206, UI_TEXT_SUB);
  }
}

static void ui_refresh_dynamic(void);

static uint16_t touch_map_axis(uint16_t raw, uint16_t raw_min, uint16_t raw_max, uint16_t out_max)
{
  uint32_t scaled;

  if (raw_max <= raw_min) {
    return 0u;
  }

  if (raw < raw_min) {
    raw = raw_min;
  }
  if (raw > raw_max) {
    raw = raw_max;
  }

  scaled = (uint32_t)(raw - raw_min) * (uint32_t)out_max;
  return (uint16_t)(scaled / (uint32_t)(raw_max - raw_min));
}

static void touch_map_to_screen(uint16_t raw_x, uint16_t raw_y, uint16_t *sx, uint16_t *sy)
{
  uint16_t mx = raw_x;
  uint16_t my = raw_y;

#if TOUCH_SWAP_XY
  {
    uint16_t t = mx;
    mx = my;
    my = t;
  }
#endif

#if TOUCH_INVERT_X
  mx = (uint16_t)(4095u - mx);
#endif

#if TOUCH_INVERT_Y
  my = (uint16_t)(4095u - my);
#endif

  if (sx != NULL) {
    *sx = touch_map_axis(mx, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, LCD_WIDTH - 1u);
  }
  if (sy != NULL) {
    *sy = touch_map_axis(my, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, LCD_HEIGHT - 1u);
  }
}

static bool touch_raw_is_reasonable(uint16_t x, uint16_t y)
{
  return (x > 8u) && (x < 4088u) && (y > 8u) && (y < 4088u);
}

static uint16_t median3_u16(uint16_t a, uint16_t b, uint16_t c)
{
  uint16_t t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  return b;
}

static uint16_t touch_read_channel(uint8_t cmd)
{
  uint8_t tx[3] = {cmd, 0x00u, 0x00u};
  uint8_t rx[3] = {0u, 0u, 0u};
  uint16_t value;
  uint32_t old_br;

  /* SPI Recovery: Nếu SPI bị kẹt ở trạng thái BUSY/ERROR (do LCD bị
     preempt giữa chừng), reset state machine trước khi giao tiếp touch. */
  if (hspi1.State != HAL_SPI_STATE_READY) {
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_SET);
    __HAL_SPI_DISABLE(&hspi1);
    hspi1.State = HAL_SPI_STATE_READY;
    __HAL_SPI_ENABLE(&hspi1);
  }

  old_br = hspi1.Instance->CR1 & SPI_CR1_BR;
  __HAL_SPI_DISABLE(&hspi1);
  MODIFY_REG(hspi1.Instance->CR1, SPI_CR1_BR, TOUCH_SPI_BR);
  __HAL_SPI_ENABLE(&hspi1);

  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_RESET);
  (void)HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3u, 50u);
  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_SET);

  __HAL_SPI_DISABLE(&hspi1);
  MODIFY_REG(hspi1.Instance->CR1, SPI_CR1_BR, old_br);
  __HAL_SPI_ENABLE(&hspi1);

  value = (uint16_t)(((uint16_t)rx[1] << 8) | rx[2]);
  value >>= 3;
  return (uint16_t)(value & 0x0FFFu);
}

static bool touch_read_raw(uint16_t *x, uint16_t *y)
{
  uint16_t x1, x2, x3, y1, y2, y3;
  uint16_t x_min, x_max, y_min, y_max;

  if ((x == NULL) || (y == NULL)) {
    return false;
  }

  if (HAL_GPIO_ReadPin(TP_IRQ_GPIO_Port, TP_IRQ_Pin) != GPIO_PIN_RESET) {
    return false;
  }

  x1 = touch_read_channel(XPT_CMD_X);
  y1 = touch_read_channel(XPT_CMD_Y);
  x2 = touch_read_channel(XPT_CMD_X);
  y2 = touch_read_channel(XPT_CMD_Y);
  x3 = touch_read_channel(XPT_CMD_X);
  y3 = touch_read_channel(XPT_CMD_Y);

  x_min = x1;
  x_max = x1;
  y_min = y1;
  y_max = y1;

  if (x2 < x_min) x_min = x2;
  if (x2 > x_max) x_max = x2;
  if (x3 < x_min) x_min = x3;
  if (x3 > x_max) x_max = x3;

  if (y2 < y_min) y_min = y2;
  if (y2 > y_max) y_max = y2;
  if (y3 < y_min) y_min = y3;
  if (y3 > y_max) y_max = y3;

  if (((x_max - x_min) > TOUCH_STABLE_SPAN_MAX) || ((y_max - y_min) > TOUCH_STABLE_SPAN_MAX)) {
    return false;
  }

  *x = median3_u16(x1, x2, x3);
  *y = median3_u16(y1, y2, y3);
  return touch_raw_is_reasonable(*x, *y);
}

/**
 * @brief  Gửi dummy command để đánh thức XPT2046 khỏi power-down mode.
 *         IC cần ít nhất 1 SPI transaction sau khi cấp nguồn để thoát
 *         chế độ ngủ. Gọi 1 lần khi khởi tạo task GUI.
 */
static void touch_wakeup_xpt2046(void)
{
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_SET);
  (void)touch_read_channel(XPT_CMD_X);
  (void)touch_read_channel(XPT_CMD_Y);
  (void)touch_read_channel(XPT_CMD_X);
  (void)touch_read_channel(XPT_CMD_Y);
}

static void ui_handle_touch_and_buttons(void)
{
  uint16_t raw_x = 0;
  uint16_t raw_y = 0;
  uint16_t sx = 0;
  uint16_t sy = 0;
  uint8_t hit_btn = 0u;
  bool pressed = touch_read_raw(&raw_x, &raw_y);

  if (pressed) {
    touch_map_to_screen(raw_x, raw_y, &sx, &sy);
    g_app.touch_x = sx;
    g_app.touch_y = sy;

    hit_btn = ui_hit_button_id(sx, sy);

    if (hit_btn != 0u) {
      g_touch_last_hit = hit_btn;
    }

    if ((g_touch_press_btn == 0u) && (hit_btn != 0u)) {
      g_touch_press_btn = hit_btn;
    }
  } else {
    if ((g_touch_press_btn != 0u) && (g_touch_press_btn == g_touch_last_hit)) {
      if (g_touch_press_btn == 1u) {
        /* === Nút PLAY được nhấn === */
        if (!g_app.play_enabled) {
          /* Bật Play -> tắt Pause (exclusive) */
          g_app.play_enabled = true;
          g_app.pause_enabled = false;
          g_ui_need_full_redraw = true; /* Vẽ lại màn hình chính nếu đang ở Data Viewer */
          /* Gửi tín hiệu cho TaskSD bắt đầu ghi file */
          osThreadFlagsSet(TaskSDHandle, SD_FLAG_PLAY_START);
        } else {
          /* Tắt Play */
          g_app.play_enabled = false;
          osThreadFlagsSet(TaskSDHandle, SD_FLAG_PLAY_STOP);
        }
      } else {
        /* === Nút PAUSE được nhấn === */
        if (!g_app.pause_enabled) {
          /* Bật Pause -> tắt Play (exclusive), yêu cầu đọc file */
          g_app.pause_enabled = true;
          g_app.play_enabled = false;
          g_sd_viewer.ready = false;
          g_sd_viewer.requested_page = 0u;
          /* Gửi tín hiệu: dừng ghi + đọc file để hiển thị */
          osThreadFlagsSet(TaskSDHandle, SD_FLAG_PLAY_STOP | SD_FLAG_READ_REQUEST);
        } else {
          /* Tắt Pause -> quay về màn hình chính */
          g_app.pause_enabled = false;
          g_ui_need_full_redraw = true;
        }
      }
    }

    g_touch_press_btn = 0u;
    g_touch_last_hit = 0u;
    g_app.touch_x = 0xFFFFu;
    g_app.touch_y = 0xFFFFu;
  }

  g_app.touch_pressed = pressed;
}

/* === Data Viewer: Layout nút điều hướng ===
 * TẤT CẢ các nút nằm ở NỬA TRÁI màn hình (safe zone)
 * vì góc phải phía dưới bị liệt cảm ứng. */
#define VIEWER_NAV_Y        288u
#define VIEWER_BACK_X       6u
#define VIEWER_PREV_X       6u
#define VIEWER_NEXT_X       72u
#define VIEWER_NAV_W        62u
#define VIEWER_NAV_H        26u
#define VIEWER_BACK_W       52u
#define VIEWER_CLEAR_X      64u    /* Cạnh nút BACK */
#define VIEWER_CLEAR_W      60u

/**
 * @brief  Xử lý touch khi đang ở màn hình Data Viewer.
 *         Nhận diện nhấn BACK/Prev/Next — tất cả nằm ở nửa trái
 *         để tránh vùng cảm ứng bị liệt ở góc phải dưới.
 */
static void ui_handle_viewer_touch(void)
{
  uint16_t raw_x = 0, raw_y = 0, sx = 0, sy = 0;
  static uint8_t v_press = 0u;
  static uint8_t v_last = 0u;
  bool pressed = touch_read_raw(&raw_x, &raw_y);

  if (pressed) {
    touch_map_to_screen(raw_x, raw_y, &sx, &sy);
    g_app.touch_x = sx;
    g_app.touch_y = sy;

    /* Kiểm tra nhấn BACK (hàng trên, y = VIEWER_NAV_Y - 30) */
    if (ui_in_rect(sx, sy, VIEWER_BACK_X, (uint16_t)(VIEWER_NAV_Y - 30u),
                   VIEWER_BACK_W, VIEWER_NAV_H)) {
      if (v_press == 0u) v_press = 5u;
      v_last = 5u;
    }
    /* Kiểm tra nhấn Prev (hàng dưới, bên trái) */
    else if (ui_in_rect(sx, sy, VIEWER_PREV_X, VIEWER_NAV_Y,
                        VIEWER_NAV_W, VIEWER_NAV_H)) {
      if (v_press == 0u) v_press = 3u;
      v_last = 3u;
    }
    /* Kiểm tra nhấn Next (hàng dưới, cạnh Prev) */
    else if (ui_in_rect(sx, sy, VIEWER_NEXT_X, VIEWER_NAV_Y,
                        VIEWER_NAV_W, VIEWER_NAV_H)) {
      if (v_press == 0u) v_press = 4u;
      v_last = 4u;
    }
    /* Kiểm tra nhấn CLEAR (Cạnh nút BACK) */
    else if (ui_in_rect(sx, sy, VIEWER_CLEAR_X, (uint16_t)(VIEWER_NAV_Y - 30u),
                        VIEWER_CLEAR_W, VIEWER_NAV_H)) {
      if (!g_app.play_enabled) { /* Bảo vệ: không cho xóa khi đang ghi */
        if (v_press == 0u) v_press = 6u;
        v_last = 6u;
      }
    }
  } else {
    if ((v_press != 0u) && (v_press == v_last)) {
      if (v_press == 5u) {
        /* BACK → thoát Data Viewer, quay về Home */
        g_app.pause_enabled = false;
        g_ui_need_full_redraw = true;
      } else if (v_press == 3u) {
        /* Prev page */
        if (g_sd_viewer.current_page > 0u) {
          g_sd_viewer.ready = false;
          g_sd_viewer.requested_page = g_sd_viewer.current_page - 1u;
          g_sd_viewer.request_page = true;
          osThreadFlagsSet(TaskSDHandle, SD_FLAG_READ_REQUEST);
        }
      } else if (v_press == 4u) {
        /* Next page */
        if ((g_sd_viewer.current_page + 1u) < g_sd_viewer.total_pages) {
          g_sd_viewer.ready = false;
          g_sd_viewer.requested_page = g_sd_viewer.current_page + 1u;
          g_sd_viewer.request_page = true;
          osThreadFlagsSet(TaskSDHandle, SD_FLAG_READ_REQUEST);
        }
      } else if (v_press == 6u) {
        /* CLEAR → Gửi lệnh xóa file */
        g_sd_viewer.ready = false;
        osThreadFlagsSet(TaskSDHandle, SD_FLAG_DELETE);
      }
    }
    v_press = 0u;
    v_last = 0u;
    g_app.touch_x = 0xFFFFu;
    g_app.touch_y = 0xFFFFu;
  }
  g_app.touch_pressed = pressed;
}

/**
 * @brief  Vẽ màn hình Data Viewer lên LCD.
 *         Layout: Header (trên) → Data list (giữa) → BACK + Prev/Next (dưới).
 *         Tất cả nút ở nửa TRÁI để tránh vùng touch chết.
 */
static void ui_display_sd_data(void)
{
  uint16_t i;
  char page_info[24];

  /* Header */
  ui_fill_gradient_rect(0u, 0u, LCD_WIDTH, 30u, RGB565(58, 50, 80), RGB565(98, 72, 140));
  lcd_display_string(60u, 8u, (const uint8_t *)"DATA VIEWER", FONT_1206, WHITE);

  /* Nền danh sách */
  lcd_fill_rect(0u, 30u, LCD_WIDTH, 226u, RGB565(248, 246, 255));

  if (!g_sd_viewer.ready) {
    lcd_display_string(60u, 130u, (const uint8_t *)"Loading...", FONT_1608, UI_ACCENT);
  } else if (g_sd_viewer.total_lines == 0u) {
    lcd_display_string(40u, 130u, (const uint8_t *)"No data found", FONT_1608, UI_TEXT_SUB);
  } else {
    /* Vẽ các dòng dữ liệu — mỗi dòng chiếm 2 hàng LCD (36px) vì chuỗi dài tự xuống hàng */
    for (i = 0u; i < g_sd_viewer.line_count; i++) {
      uint16_t y_pos = 34u + (i * 36u);
      if ((i % 2u) == 0u) {
        lcd_fill_rect(2u, y_pos, 236u, 32u, RGB565(240, 236, 252));
      } else {
        lcd_fill_rect(2u, y_pos, 236u, 32u, RGB565(252, 250, 255));
      }
      /* Format: HH:MM:SS|DD/MM/YYYY|XX.X C|T:XXXX|A:XXXX (~43 ký tự)
         Vượt 240px nên tự xuống hàng — background 32px chứa đủ 2 dòng FONT_1206 */
      lcd_display_string(4, (uint16_t)(y_pos + 2u),
                         (const uint8_t *)g_sd_viewer.lines[i],
                         FONT_1206, UI_TEXT_MAIN);
    }
  }

  /* === Thanh điều hướng — tất cả nằm bên TRÁI === */

  /* Nút BACK (hàng trên thanh nav, x=6, y = NAV_Y - 30) */
  lcd_fill_rect(0u, (uint16_t)(VIEWER_NAV_Y - 32u), LCD_WIDTH, 30u, RGB565(238, 232, 250));
  ui_fill_gradient_rect(VIEWER_BACK_X, (uint16_t)(VIEWER_NAV_Y - 30u),
                        VIEWER_BACK_W, VIEWER_NAV_H,
                        RGB565(240, 100, 100), RGB565(200, 60, 60));
  lcd_display_string((uint16_t)(VIEWER_BACK_X + 8u), (uint16_t)(VIEWER_NAV_Y - 24u),
                     (const uint8_t *)"BACK", FONT_1206, WHITE);

  /* Nút CLEAR (Màu đỏ, chỉ hiện khi không ghi) */
  if (!g_app.play_enabled) {
    ui_fill_gradient_rect(VIEWER_CLEAR_X, (uint16_t)(VIEWER_NAV_Y - 30u),
                          VIEWER_CLEAR_W, VIEWER_NAV_H, UI_ERR, RGB565(180, 0, 0));
    lcd_display_string((uint16_t)(VIEWER_CLEAR_X + 6u), (uint16_t)(VIEWER_NAV_Y - 24u),
                       (const uint8_t *)"CLEAR", FONT_1206, WHITE);
  }

  /* Số trang (Dịch sang X=130 để tránh đè lên nút CLEAR) */
  (void)sprintf(page_info, "Page %u/%u",
                (unsigned)(g_sd_viewer.current_page + 1u),
                (unsigned)(g_sd_viewer.total_pages > 0u ? g_sd_viewer.total_pages : 1u));
  lcd_display_string(130u, (uint16_t)(VIEWER_NAV_Y - 24u),
                     (const uint8_t *)page_info, FONT_1206, UI_TEXT_MAIN);

  /* Thanh phân trang (hàng dưới) */
  lcd_fill_rect(0u, VIEWER_NAV_Y, LCD_WIDTH, 32u, RGB565(230, 224, 245));

  /* Nút Prev (trái) */
  if (g_sd_viewer.current_page > 0u) {
    ui_fill_gradient_rect(VIEWER_PREV_X, VIEWER_NAV_Y + 2u, VIEWER_NAV_W, VIEWER_NAV_H,
                          RGB565(200, 180, 240), RGB565(160, 130, 210));
    lcd_display_string((uint16_t)(VIEWER_PREV_X + 12u), VIEWER_NAV_Y + 8u,
                       (const uint8_t *)"<Prev", FONT_1206, WHITE);
  } else {
    lcd_fill_rect(VIEWER_PREV_X, VIEWER_NAV_Y + 2u, VIEWER_NAV_W, VIEWER_NAV_H,
                  RGB565(218, 214, 228));
    lcd_display_string((uint16_t)(VIEWER_PREV_X + 12u), VIEWER_NAV_Y + 8u,
                       (const uint8_t *)"<Prev", FONT_1206, RGB565(170, 162, 180));
  }

  /* Nút Next (cạnh Prev, vẫn ở nửa trái) */
  if ((g_sd_viewer.current_page + 1u) < g_sd_viewer.total_pages) {
    ui_fill_gradient_rect(VIEWER_NEXT_X, VIEWER_NAV_Y + 2u, VIEWER_NAV_W, VIEWER_NAV_H,
                          RGB565(200, 180, 240), RGB565(160, 130, 210));
    lcd_display_string((uint16_t)(VIEWER_NEXT_X + 8u), VIEWER_NAV_Y + 8u,
                       (const uint8_t *)"Next>", FONT_1206, WHITE);
  } else {
    lcd_fill_rect(VIEWER_NEXT_X, VIEWER_NAV_Y + 2u, VIEWER_NAV_W, VIEWER_NAV_H,
                  RGB565(218, 214, 228));
    lcd_display_string((uint16_t)(VIEWER_NEXT_X + 8u), VIEWER_NAV_Y + 8u,
                       (const uint8_t *)"Next>", FONT_1206, RGB565(170, 162, 180));
  }
}

static void ui_refresh_dynamic(void)
{
  /* Vẽ lại nhãn tiêu đề 2 cột đo lường */
  lcd_fill_rect(16, 52, 100, 20, UI_CARD);
  lcd_display_string(22, 58, (const uint8_t *)"Chip Temp", FONT_1206, UI_TEXT_SUB);
  lcd_fill_rect(122, 52, 100, 20, UI_CARD);
  lcd_display_string(128, 58, (const uint8_t *)"ADC Values", FONT_1206, UI_TEXT_SUB);

  ui_draw_temp_value(g_app.temp_x10);
  ui_draw_adc_values(g_app.adc_raw_temp, g_app.adc_raw_analog);
  ui_draw_button(BTN_PLAY_X, BTN_PLAY_Y, (const uint8_t *)"PLAY", g_app.play_enabled);
  ui_draw_button(BTN_PAUSE_X, BTN_PAUSE_Y, (const uint8_t *)"PAUSE", g_app.pause_enabled);
  ui_draw_status(g_app.play_enabled, g_app.pause_enabled, g_app.sd_state, g_app.sd_code);

  /* --- FOOTER STATUS BAR (DATE & TIME) --- */
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  char footer_str[32];
  
  /* Lấy thời gian và ngày tháng hiện tại từ RTC */
  HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
  HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  
  /* Định dạng chuỗi: HH:MM:SS|DD/MM/YYYY */
  sprintf(footer_str, "%02u:%02u:%02u|%02u/%02u/%04u", 
          (unsigned)sTime.Hours, (unsigned)sTime.Minutes, (unsigned)sTime.Seconds, 
          (unsigned)sDate.Date, (unsigned)sDate.Month, (unsigned)(2000u + sDate.Year));
          
  /* Xóa vùng nền chân trang (Y=305 đến 320) trước khi vẽ để chống nhòe (flicker/overlap) khi giây thay đổi */
  lcd_fill_rect(0, 305, LCD_WIDTH, 15, UI_BG_BOTTOM);
  
  /* Căn giữa chuỗi: FONT_1206 (Rộng 6px/ký tự). Chuỗi "HH:MM:SS|DD/MM/YYYY" có 19 ký tự.
     Độ rộng chuỗi = 19 * 6 = 114px.
     Tọa độ X căn giữa = (240 - 114) / 2 = 63 */
  lcd_display_string(63, 305, (const uint8_t *)footer_str, FONT_1206, UI_TEXT_SUB);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_RTC_Init();
  MX_SDIO_SD_Init();
  MX_SPI1_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */

  /* Khởi động ADC DMA với 3 kênh: Internal Temp, VREFINT, IN10 */
  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_dma, 3u) != HAL_OK)
  {
    Error_Handler();
  }

  lcd_init();
  ui_draw_static();
  ui_refresh_dynamic();

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();
  /* Create the mutex(es) */
  /* creation of MutexSPI */
  MutexSPIHandle = osMutexNew(&MutexSPI_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of QueueData */
  QueueDataHandle = osMessageQueueNew (10, sizeof(uint16_t), &QueueData_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of TaskGUI */
  TaskGUIHandle = osThreadNew(StartTaskGUI, NULL, &TaskGUI_attributes);

  /* creation of TaskSensor */
  TaskSensorHandle = osThreadNew(StartTaskSensor, NULL, &TaskSensor_attributes);

  /* creation of TaskSD */
  TaskSDHandle = osThreadNew(StartTaskSD, NULL, &TaskSD_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ENABLE;
  hadc1.Init.ContinuousConvMode = ENABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 3;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_TEMPSENSOR;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_112CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_VREFINT;
  sConfig.Rank = 2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = 3;
  sConfig.SamplingTime = ADC_SAMPLETIME_84CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_THURSDAY;
  sDate.Month = RTC_MONTH_APRIL;
  sDate.Date = 0x9;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */
  /* --- TỰ ĐỘNG CẬP NHẬT NGÀY/GIỜ THEO THỜI GIAN BIÊN DỊCH ---
   * Giúp đồng hồ luôn đúng lúc vừa nạp code xong.
   * Macro __TIME__ có định dạng "HH:MM:SS" */
  const char *t = __TIME__;
  sTime.Hours   = (uint8_t)((t[0] - '0') * 10 + (t[1] - '0'));
  sTime.Minutes = (uint8_t)((t[3] - '0') * 10 + (t[4] - '0'));
  sTime.Seconds = (uint8_t)((t[6] - '0') * 10 + (t[7] - '0'));
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);

  /* Macro __DATE__ có định dạng "Mmm dd yyyy" (vd: "Apr 26 2026") */
  const char *d = __DATE__;
  uint8_t month = 1;
  if (d[0] == 'J') {
      if (d[1] == 'a') month = 1;
      else if (d[2] == 'n') month = 6;
      else month = 7;
  } else if (d[0] == 'F') month = 2;
  else if (d[0] == 'M') {
      if (d[2] == 'r') month = 3;
      else month = 5;
  } else if (d[0] == 'A') {
      if (d[1] == 'p') month = 4;
      else month = 8;
  } else if (d[0] == 'S') month = 9;
  else if (d[0] == 'O') month = 10;
  else if (d[0] == 'N') month = 11;
  else if (d[0] == 'D') month = 12;
  
  sDate.Date = (uint8_t)((d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0'));
  sDate.Month = month;
  sDate.Year = (uint8_t)((d[9] - '0') * 10 + (d[10] - '0')); /* Lưu ý: Tính theo mốc năm 2000 */
  HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SDIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_SDIO_SD_Init(void)
{

  /* USER CODE BEGIN SDIO_Init 0 */

  /* USER CODE END SDIO_Init 0 */

  /* USER CODE BEGIN SDIO_Init 1 */

  /* USER CODE END SDIO_Init 1 */
  hsd.Instance = SDIO;
  hsd.Init.ClockEdge = SDIO_CLOCK_EDGE_RISING;
  hsd.Init.ClockBypass = SDIO_CLOCK_BYPASS_DISABLE;
  hsd.Init.ClockPowerSave = SDIO_CLOCK_POWER_SAVE_DISABLE;
  hsd.Init.BusWide = SDIO_BUS_WIDE_1B;
  hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd.Init.ClockDiv = 4;
  /* USER CODE BEGIN SDIO_Init 2 */
  hsd.Init.ClockDiv = 10;

  /* USER CODE END SDIO_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USB_OTG_FS Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_OTG_FS_PCD_Init(void)
{

  /* USER CODE BEGIN USB_OTG_FS_Init 0 */

  /* USER CODE END USB_OTG_FS_Init 0 */

  /* USER CODE BEGIN USB_OTG_FS_Init 1 */

  /* USER CODE END USB_OTG_FS_Init 1 */
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 4;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_OTG_FS_Init 2 */

  /* USER CODE END USB_OTG_FS_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
  /* DMA2_Stream6_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LCD_RST_Pin|LCD_BKL_Pin|LCD_DC_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LCD_CS_Pin|TP_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pins : LCD_RST_Pin LCD_BKL_Pin LCD_CS_Pin LCD_DC_Pin
                           TP_CS_Pin */
  GPIO_InitStruct.Pin = LCD_RST_Pin|LCD_BKL_Pin|LCD_CS_Pin|LCD_DC_Pin
                          |TP_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : TP_IRQ_Pin */
  GPIO_InitStruct.Pin = TP_IRQ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(TP_IRQ_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

static uint16_t sd_error_to_code(uint32_t err)
{
  if (err == HAL_SD_ERROR_NONE) {
    if (g_sd_init_stage != 0u) {
      return (uint16_t)(140u + g_sd_init_stage);
    }
    switch (g_sd_last_hal_status) {
    case HAL_OK:      return 1u;
    case HAL_ERROR:   return 101u;
    case HAL_BUSY:    return 102u;
    case HAL_TIMEOUT: return 103u;
    default:          return 199u;
    }
  }
  if (err & HAL_SD_ERROR_CMD_RSP_TIMEOUT) return 11u;
  if (err & HAL_SD_ERROR_CMD_CRC_FAIL) return 12u;
  if (err & HAL_SD_ERROR_DATA_TIMEOUT) return 13u;
  if (err & HAL_SD_ERROR_DATA_CRC_FAIL) return 14u;
  if (err & HAL_SD_ERROR_TX_UNDERRUN) return 15u;
  if (err & HAL_SD_ERROR_RX_OVERRUN) return 16u;
  if (err & HAL_SD_ERROR_ADDR_MISALIGNED) return 17u;
  if (err & HAL_SD_ERROR_ADDR_OUT_OF_RANGE) return 18u;
  if (err & HAL_SD_ERROR_REQUEST_NOT_APPLICABLE) return 19u;
  if (err & HAL_SD_ERROR_INVALID_VOLTRANGE) return 20u;
  if (err & HAL_SD_ERROR_DMA) return 21u;
  return 99u;
}

static uint16_t sd_fresult_to_code(FRESULT fres)
{
  return (uint16_t)(200u + (uint16_t)fres);
}

static bool sd_apply_profile(uint32_t bus_wide, uint8_t clock_div)
{
  HAL_StatusTypeDef st;

  hsd.Init.ClockDiv = clock_div;
  st = HAL_SD_ConfigWideBusOperation(&hsd, bus_wide);
  g_sd_last_hal_status = st;

  if (st == HAL_OK) {
    g_sd_last_hal_error = HAL_SD_ERROR_NONE;
    return true;
  }

  g_sd_last_hal_error = HAL_SD_GetError(&hsd);
  return false;
}

static bool sd_probe_filesystem(void)
{
  DWORD free_clusters = 0u;
  FATFS *fs_ptr = NULL;
  FRESULT fres;

  fres = f_getfree((const TCHAR *)SDPath, &free_clusters, &fs_ptr);
  (void)free_clusters;
  return (fres == FR_OK) && (fs_ptr != NULL);
}

/* Override weak BSP_SD_Init to improve reliability across marginal cards/wiring. */
uint8_t BSP_SD_Init(void)
{
  uint32_t retry;
  HAL_StatusTypeDef st;

  g_sd_last_hal_error = HAL_SD_ERROR_NONE;
  g_sd_last_hal_status = HAL_OK;
  g_sd_init_stage = 0u;

  if (BSP_SD_IsDetected() != SD_PRESENT) {
    g_sd_init_stage = 1u;
    g_sd_last_hal_error = HAL_SD_ERROR_REQUEST_NOT_APPLICABLE;
    g_sd_last_hal_status = HAL_ERROR;
    return MSD_ERROR;
  }

  for (retry = 0u; retry < 3u; retry++) {
    (void)HAL_SD_DeInit(&hsd);
    HAL_Delay(10);

    st = HAL_SD_Init(&hsd);
    g_sd_last_hal_status = st;
    if (st == HAL_OK) {
#if SD_PREFER_1BIT_MODE
      /* On custom boards, 1-bit mode is often more tolerant to signal integrity issues. */
      g_sd_init_stage = 3u;
      st = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_1B);
      g_sd_last_hal_status = st;
      if (st == HAL_OK) {
        g_sd_init_stage = 0u;
        g_sd_last_hal_error = HAL_SD_ERROR_NONE;
        return MSD_OK;
      }

      /* Fallback to 4-bit if 1-bit setup unexpectedly fails. */
      g_sd_init_stage = 2u;
      st = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B);
      g_sd_last_hal_status = st;
      if (st == HAL_OK) {
        g_sd_init_stage = 0u;
        g_sd_last_hal_error = HAL_SD_ERROR_NONE;
        return MSD_OK;
      }
#else
      g_sd_init_stage = 2u;
      st = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B);
      g_sd_last_hal_status = st;
      if (st == HAL_OK) {
        g_sd_init_stage = 0u;
        g_sd_last_hal_error = HAL_SD_ERROR_NONE;
        return MSD_OK;
      }

      /* Some cards/boards are only stable in 1-bit mode. */
      g_sd_init_stage = 3u;
      st = HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_1B);
      g_sd_last_hal_status = st;
      if (st == HAL_OK) {
        g_sd_init_stage = 0u;
        g_sd_last_hal_error = HAL_SD_ERROR_NONE;
        return MSD_OK;
      }
#endif
    }

    if (st != HAL_OK) {
      g_sd_init_stage = 4u;
    }
    g_sd_last_hal_error = HAL_SD_GetError(&hsd);
    HAL_Delay(25);
  }

  return MSD_ERROR;
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartTaskGUI */
/**
  * @brief  Function implementing the TaskGUI thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartTaskGUI */
void StartTaskGUI(void *argument)
{
  /* USER CODE BEGIN 5 */
  bool draw_init = false;
  int16_t last_temp_x10 = 0;
  bool last_play_enabled = false;
  bool last_pause_enabled = false;
  bool last_touch_pressed = false;
  uint16_t last_touch_x = 0xFFFFu;
  uint16_t last_touch_y = 0xFFFFu;
  sd_state_t last_sd_state = SD_STATE_CHECKING;
  uint16_t last_sd_code = 0u;
  uint32_t last_periodic_redraw = 0u;
  /* Biến theo dõi trạng thái Data Viewer để phát hiện thay đổi */
  bool viewer_was_active = false;
  bool last_viewer_ready = false;
  uint16_t last_adc_analog = 0u;

  /* Đánh thức XPT2046 khỏi power-down mode (1 lần duy nhất khi boot) */
  if (osMutexAcquire(MutexSPIHandle, 200) == osOK) {
    touch_wakeup_xpt2046();
    (void)osMutexRelease(MutexSPIHandle);
  }

  /* Infinite loop */
  for(;;)
  {
    if (osMutexAcquire(MutexSPIHandle, 50) == osOK)
    {
      /* ====================================================================
       * CHẾ ĐỘ DATA VIEWER (khi pause_enabled == true)
       * - Dùng hàm touch riêng (ui_handle_viewer_touch) để xử lý Prev/Next
       * - Vẽ giao diện Data Viewer thay vì màn hình chính
       * - Không gọi ui_refresh_dynamic() để tránh xung đột vẽ
       * ==================================================================== */
      if (g_app.pause_enabled) {
        /* Lần đầu vào Data Viewer: vẽ toàn bộ overlay */
        if (!viewer_was_active) {
          viewer_was_active = true;
          last_viewer_ready = false;
          ui_display_sd_data();
        }

        /* Xử lý touch cho phân trang (Prev/Next/thoát) */
        ui_handle_viewer_touch();

        /* Cập nhật khi dữ liệu mới sẵn sàng (TaskSD đọc xong) */
        if (g_sd_viewer.ready != last_viewer_ready) {
          last_viewer_ready = g_sd_viewer.ready;
          ui_display_sd_data();
        }

        /* Cập nhật định kỳ để hiển thị trạng thái loading */
        if ((HAL_GetTick() - last_periodic_redraw) >= 300u) {
          last_periodic_redraw = HAL_GetTick();
          ui_display_sd_data();
        }
      }
      /* ====================================================================
       * CHẾ ĐỘ BÌNH THƯỜNG (màn hình chính)
       * ==================================================================== */
      else {
        bool need_redraw = false;

        /* Nếu vừa thoát Data Viewer, vẽ lại toàn bộ màn hình chính */
        if (viewer_was_active || g_ui_need_full_redraw) {
          viewer_was_active = false;
          g_ui_need_full_redraw = false;
          ui_draw_static();
          need_redraw = true;
        }

        ui_handle_touch_and_buttons();

        if (!draw_init) {
          draw_init = true;
          last_temp_x10 = g_app.temp_x10;
          last_adc_analog = g_app.adc_raw_analog;
          last_play_enabled = g_app.play_enabled;
          last_pause_enabled = g_app.pause_enabled;
          last_touch_pressed = g_app.touch_pressed;
          last_touch_x = g_app.touch_x;
          last_touch_y = g_app.touch_y;
          last_sd_state = g_app.sd_state;
          last_sd_code = g_app.sd_code;
          last_periodic_redraw = HAL_GetTick();
          need_redraw = true;
        }

        if (g_app.temp_x10 != last_temp_x10) {
          last_temp_x10 = g_app.temp_x10;
          need_redraw = true;
        }

        if (g_app.adc_raw_analog != last_adc_analog) {
          last_adc_analog = g_app.adc_raw_analog;
          need_redraw = true;
        }

        if (g_app.play_enabled != last_play_enabled) {
          last_play_enabled = g_app.play_enabled;
          need_redraw = true;
        }

        if (g_app.pause_enabled != last_pause_enabled) {
          last_pause_enabled = g_app.pause_enabled;
          need_redraw = true;
        }

        if (g_app.sd_state != last_sd_state) {
          last_sd_state = g_app.sd_state;
          need_redraw = true;
        }

        if (g_app.sd_code != last_sd_code) {
          last_sd_code = g_app.sd_code;
          need_redraw = true;
        }

        if (g_app.touch_pressed != last_touch_pressed) {
          last_touch_pressed = g_app.touch_pressed;
          need_redraw = true;
        }

        if (g_app.touch_pressed) {
          uint16_t dx = (g_app.touch_x > last_touch_x) ? (g_app.touch_x - last_touch_x) : (last_touch_x - g_app.touch_x);
          uint16_t dy = (g_app.touch_y > last_touch_y) ? (g_app.touch_y - last_touch_y) : (last_touch_y - g_app.touch_y);
          if ((dx >= 3u) || (dy >= 3u)) {
            last_touch_x = g_app.touch_x;
            last_touch_y = g_app.touch_y;
            need_redraw = true;
          }
        }

        if ((HAL_GetTick() - last_periodic_redraw) >= 150u) {
          last_periodic_redraw = HAL_GetTick();
          need_redraw = true;
        }

        if (need_redraw) {
          ui_refresh_dynamic();
        }
      }

      (void)osMutexRelease(MutexSPIHandle);
    }

    osDelay(2);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartTaskSensor */
/**
* @brief Function implementing the TaskSensor thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskSensor */
void StartTaskSensor(void *argument)
{
  /* USER CODE BEGIN StartTaskSensor */
  uint16_t raw_int, raw_vref, raw_analog;
  uint16_t filt_int = 0u, filt_vref = 0u, filt_analog = 0u;
  bool filter_primed = false;

  /* Infinite loop */
  for(;;)
  {
    /* Đọc dữ liệu thô từ DMA (Rank 1: Temp, Rank 2: VREFINT, Rank 3: IN10) */
    raw_int    = g_adc_dma[0];
    raw_vref   = g_adc_dma[1];
    raw_analog = g_adc_dma[2];

    if (!filter_primed) {
      filt_int = raw_int; filt_vref = raw_vref; filt_analog = raw_analog;
      filter_primed = true;
    } else {
      /* Bộ lọc Exponential Moving Average (EMA) - Giảm nhiễu */
      filt_int    = (uint16_t)(((uint32_t)filt_int * 7u + (uint32_t)raw_int * 3u) / 10u);
      filt_vref   = (uint16_t)(((uint32_t)filt_vref * 7u + (uint32_t)raw_vref * 3u) / 10u);
      filt_analog = (uint16_t)(((uint32_t)filt_analog * 7u + (uint32_t)raw_analog * 3u) / 10u);
    }

    /* Tính toán và lưu nhiệt độ chip nội (Đã bù trừ sụt áp nguồn VREFINT) */
    g_app.adc_raw_temp = filt_int;
    g_app.adc_raw_analog = filt_analog;
    g_app.temp_x10 = adc_temp_x10_from_raw(filt_int, filt_vref);

    osDelay(20);
  }
  /* USER CODE END StartTaskSensor */
}

/* USER CODE BEGIN Header_StartTaskSD */
/**
* @brief Function implementing the TaskSD thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTaskSD */
void StartTaskSD(void *argument)
{
  /* USER CODE BEGIN StartTaskSD */
  bool sd_hw_ready = false;
#if SD_AUTO_FORMAT_ON_NOFS
  uint8_t nofs_format_retry = 0u;
#endif
#if SD_AUTOTUNE_ENABLE
  uint8_t tune_idx = 0u;
  uint8_t tune_best_idx = 0u;
  bool tune_done = false;
  uint32_t tune_last_tick = 0u;
#endif
  FRESULT fres;
#if SD_AUTO_FORMAT_ON_NOFS
  static uint8_t mkfs_work[SD_MKFS_WORKBUF_SIZE] __attribute__((aligned(4)));
#endif

  /* === Biến cục bộ cho tính năng Play/Pause === */
  bool is_logging = false;        /* Đang trong chế độ ghi file? */
  uint32_t last_log_sec = 0u;     /* Thời điểm ghi lần cuối (tính bằng giây RTC) */
  FIL log_file;                   /* File object riêng cho logging (không dùng SDFile toàn cục) */

  /* Infinite loop */
  for(;;)
  {
    /* ================================================================
     * PHASE 1: Khởi tạo phần cứng SD Card (giữ nguyên logic gốc)
     * ================================================================ */
    if (!sd_hw_ready) {
#if SD_AUTOTUNE_ENABLE
      hsd.Init.ClockDiv = g_sd_tune_clock_divs[0];
#endif
      if (BSP_SD_Init() == MSD_OK) {
        sd_hw_ready = true;
#if SD_AUTO_FORMAT_ON_NOFS
      nofs_format_retry = 0u;
#endif
#if SD_AUTOTUNE_ENABLE
        tune_idx = 0u;
        tune_best_idx = 0u;
        tune_done = false;
        tune_last_tick = HAL_GetTick();
#endif
      } else {
        g_app.sd_state = SD_STATE_ERROR;
        g_app.sd_code = sd_error_to_code(g_sd_last_hal_error);
        osDelay(600);
        continue;
      }
    }

    if (BSP_SD_GetCardState() != SD_TRANSFER_OK) {
      g_app.sd_state = SD_STATE_ERROR;
      g_app.sd_code = 2u;
      sd_hw_ready = false;
      is_logging = false;
#if SD_AUTO_FORMAT_ON_NOFS
      nofs_format_retry = 0u;
#endif
    #if SD_AUTOTUNE_ENABLE
      tune_idx = 0u;
      tune_best_idx = 0u;
      tune_done = false;
    #endif
      osDelay(600);
      continue;
    }

    /* ================================================================
     * PHASE 2: Mount filesystem (giữ nguyên logic gốc)
     * ================================================================ */
    g_app.sd_state = SD_STATE_CHECKING;
    g_app.sd_code = 91u; /* Mounting */

    fres = f_mount(&SDFatFS, (TCHAR const *)SDPath, 1);
    if (fres == FR_OK) {
      g_app.sd_state = SD_STATE_MOUNTED;
      g_app.sd_code = 0u;

      /* ==============================================================
       * PHASE 3: VÒNG LẶP SỰ KIỆN SAU KHI MOUNT THÀNH CÔNG
       *
       * BUG FIX: Phiên bản cũ gọi f_mount() MỖI vòng lặp (mỗi 3s)
       * gây block SDIO ~100ms/lần → hệ thống bị giật/treo.
       * Giải pháp: Ở lại vòng lặp trong (inner loop) khi đã mount,
       * chỉ thoát ra ngoài khi card bị lỗi/rút.
       *
       * Sử dụng SD_FLAGS_VALID() để loại bỏ mã lỗi CMSIS-RTOS
       * (0xFFFFFFFx) có bit thấp trùng với SD_FLAG_* → phantom trigger.
       * ============================================================== */
      while (g_app.sd_state == SD_STATE_MOUNTED)
      {
        uint32_t flags;
        static uint32_t last_health_tick = 0u;

        /* Chờ tín hiệu hoặc timeout 100ms để quét liên tục */
        flags = osThreadFlagsWait(SD_FLAG_ALL, osFlagsWaitAny, 100u);

        /* Kiểm tra sức khỏe card định kỳ (mỗi ~10s) */
        if ((HAL_GetTick() - last_health_tick) >= 10000u) {
          last_health_tick = HAL_GetTick();
          if (BSP_SD_GetCardState() != SD_TRANSFER_OK) {
            g_app.sd_state = SD_STATE_ERROR;
            g_app.sd_code = 2u;
            sd_hw_ready = false;
            is_logging = false;
            break; /* Thoát inner loop → outer loop sẽ re-init */
          }
        }

#if SD_AUTOTUNE_ENABLE
        if (!tune_done && ((HAL_GetTick() - tune_last_tick) >= SD_AUTOTUNE_STEP_MS)) {
          uint8_t next_idx = (uint8_t)(tune_idx + 1u);
          tune_last_tick = HAL_GetTick();
          if (next_idx < SD_TUNE_PROFILE_COUNT) {
            if (sd_apply_profile(SDIO_BUS_WIDE_1B, g_sd_tune_clock_divs[next_idx]) && sd_probe_filesystem()) {
              tune_idx = next_idx;
              tune_best_idx = next_idx;
            } else {
              (void)sd_apply_profile(SDIO_BUS_WIDE_1B, g_sd_tune_clock_divs[tune_best_idx]);
              tune_idx = tune_best_idx;
              tune_done = true;
            }
          } else {
            tune_done = true;
          }
        }
#endif

        /* Lấy thời gian RTC hiện tại */
        RTC_TimeTypeDef rtc_time = {0};
        RTC_DateTypeDef rtc_date = {0};
        HAL_RTC_GetTime(&hrtc, &rtc_time, RTC_FORMAT_BIN);
        HAL_RTC_GetDate(&hrtc, &rtc_date, RTC_FORMAT_BIN);
        
        uint32_t current_sec = (uint32_t)rtc_time.Hours * 3600u + (uint32_t)rtc_time.Minutes * 60u + rtc_time.Seconds;
        uint32_t elapsed_sec = (current_sec >= last_log_sec) ? (current_sec - last_log_sec) : (current_sec + 86400u - last_log_sec);

        /* === Xử lý flags CHỈ KHI hợp lệ (không phải mã lỗi CMSIS) === */
        if (SD_FLAGS_VALID(flags)) {
          if (flags & SD_FLAG_PLAY_START) {
            is_logging = true;
            last_log_sec = current_sec; /* Ghi đè mốc thời gian */
            elapsed_sec = 3u; /* Ép ghi file ngay lập tức */
          }
          if (flags & SD_FLAG_PLAY_STOP) {
            is_logging = false;
          }
          if (flags & SD_FLAG_DELETE) {
            is_logging = false; /* Bảo vệ kép */
            g_app.play_enabled = false;
            fres = f_unlink("data.txt");
            g_sd_viewer.total_lines = 0u;
            g_sd_viewer.total_pages = 1u;
            g_sd_viewer.current_page = 0u;
            g_sd_viewer.ready = true;
          }
        }

        /* === GHI DỮ LIỆU mỗi 3s thật (dựa vào RTC) khi is_logging === */
        if (is_logging && (elapsed_sec >= 3u)) {
          last_log_sec = current_sec;

          int16_t snap_temp = g_app.temp_x10;
          uint16_t snap_adc = g_app.adc_raw_temp;
          uint16_t snap_analog = g_app.adc_raw_analog;

          fres = f_open(&log_file, "data.txt", FA_OPEN_APPEND | FA_WRITE);
          if (fres == FR_OK) {
            /* Format: HH:MM:SS|DD/MM/YYYY|XX.X C|T:XXXX|A:XXXX
               T: = ADC raw nhiệt độ chip, A: = ADC raw Analog Test Board */
            f_printf(&log_file, "%02u:%02u:%02u|%02u/%02u/%04u|%d.%u C|T:%u|A:%u\n",
                     (unsigned)rtc_time.Hours, (unsigned)rtc_time.Minutes, (unsigned)rtc_time.Seconds,
                     (unsigned)rtc_date.Date, (unsigned)rtc_date.Month, (unsigned)(2000u + rtc_date.Year),
                     (int)(snap_temp / 10), (unsigned)((snap_temp < 0 ? -snap_temp : snap_temp) % 10u),
                     (unsigned)snap_adc, (unsigned)snap_analog);
            f_sync(&log_file);
            f_close(&log_file);
          } else {
            /* Lỗi ghi → thoát inner loop để re-mount */
            g_app.sd_state = SD_STATE_ERROR;
            g_app.sd_code = sd_fresult_to_code(fres);
            break;
          }
        }

        /* === ĐỌC FILE cho Data Viewer (Mới nhất lên đầu) === */
        if (SD_FLAGS_VALID(flags) && (flags & SD_FLAG_READ_REQUEST)) {
          FIL read_file;
          uint16_t target_page = g_sd_viewer.requested_page;
          uint16_t line_idx = 0u, total_count = 0u;
          char tmp_line[SD_VIEWER_LINE_MAX_LEN];

          fres = f_open(&read_file, "data.txt", FA_READ);
          if (fres == FR_OK) {
            while (f_gets(tmp_line, sizeof(tmp_line), &read_file) != NULL) {
              total_count++;
            }
            f_close(&read_file);

            g_sd_viewer.total_lines = total_count;
            g_sd_viewer.total_pages = (total_count + SD_VIEWER_LINES_PER_PAGE - 1u)
                                      / SD_VIEWER_LINES_PER_PAGE;
            if (g_sd_viewer.total_pages == 0u) g_sd_viewer.total_pages = 1u;
            if (target_page >= g_sd_viewer.total_pages) {
              target_page = g_sd_viewer.total_pages - 1u;
            }

            /* Tính vị trí đọc ngược: trang 0 = các dòng cuối file (mới nhất).
               line_end_excl: dòng cuối (exclusive) tính từ đầu file.
               line_start: dòng đầu tiên cần đọc trong file. */
            uint16_t line_end_excl = total_count - (target_page * SD_VIEWER_LINES_PER_PAGE);
            uint16_t lines_this_page = (line_end_excl >= SD_VIEWER_LINES_PER_PAGE)
                                        ? SD_VIEWER_LINES_PER_PAGE
                                        : line_end_excl;
            uint16_t line_start = line_end_excl - lines_this_page;

            fres = f_open(&read_file, "data.txt", FA_READ);
            if (fres == FR_OK) {
              uint16_t cur_line = 0u;
              while (f_gets(tmp_line, sizeof(tmp_line), &read_file) != NULL) {
                if (cur_line >= line_start && line_idx < lines_this_page) {
                  uint16_t len = 0u;
                  while (tmp_line[len] != '\0' && len < (SD_VIEWER_LINE_MAX_LEN - 1u)) len++;
                  while (len > 0u && (tmp_line[len-1u]=='\n' || tmp_line[len-1u]=='\r'))
                    tmp_line[--len] = '\0';
                  uint16_t j;
                  for (j = 0u; j < len && j < (SD_VIEWER_LINE_MAX_LEN - 1u); j++)
                    g_sd_viewer.lines[line_idx][j] = tmp_line[j];
                  g_sd_viewer.lines[line_idx][j] = '\0';
                  line_idx++;
                }
                cur_line++;
                if (line_idx >= lines_this_page) break;
              }
              f_close(&read_file);

              /* Đảo ngược thứ tự: dòng cuối file lên đầu buffer */
              {
                uint16_t lo = 0u;
                uint16_t hi = (line_idx > 0u) ? (line_idx - 1u) : 0u;
                while (lo < hi) {
                  char swap_buf[SD_VIEWER_LINE_MAX_LEN];
                  uint16_t k;
                  for (k = 0u; k < SD_VIEWER_LINE_MAX_LEN; k++)
                    swap_buf[k] = g_sd_viewer.lines[lo][k];
                  for (k = 0u; k < SD_VIEWER_LINE_MAX_LEN; k++)
                    g_sd_viewer.lines[lo][k] = g_sd_viewer.lines[hi][k];
                  for (k = 0u; k < SD_VIEWER_LINE_MAX_LEN; k++)
                    g_sd_viewer.lines[hi][k] = swap_buf[k];
                  lo++;
                  hi--;
                }
              }
            }
          } else {
            g_sd_viewer.total_lines = 0u;
            g_sd_viewer.total_pages = 1u;
          }
          g_sd_viewer.line_count = line_idx;
          g_sd_viewer.current_page = target_page;
          g_sd_viewer.request_page = false;
          g_sd_viewer.ready = true;
        }
      } /* end while (MOUNTED) */

    } else if (fres == FR_NO_FILESYSTEM) {
      /* Card is reachable but not FAT-compatible yet. Optionally auto-format to FAT32. */
#if SD_AUTO_FORMAT_ON_NOFS
      if (nofs_format_retry < SD_NOFS_FORMAT_MAX_RETRY) {
        nofs_format_retry++;
        g_app.sd_state = SD_STATE_CHECKING;
        g_app.sd_code = 90u; /* Formatting */

        (void)f_mount(NULL, (TCHAR const *)SDPath, 0);

        /* Re-init SD before mkfs to reduce transient NOFS after first flash/insert. */
        (void)HAL_SD_DeInit(&hsd);
        HAL_Delay(15);
        if (BSP_SD_Init() != MSD_OK) {
          g_app.sd_state = SD_STATE_ERROR;
          g_app.sd_code = sd_error_to_code(g_sd_last_hal_error);
          sd_hw_ready = false;
          osDelay(600);
          continue;
        }

        fres = f_mkfs((const TCHAR *)SDPath, FM_FAT32, 0u, mkfs_work, (UINT)sizeof(mkfs_work));
        if (fres == FR_OK) {
          HAL_Delay(SD_POST_MKFS_SETTLE_MS);
          fres = f_mount(&SDFatFS, (TCHAR const *)SDPath, 1);
          if (fres == FR_OK) {
            g_app.sd_state = SD_STATE_MOUNTED;
            g_app.sd_code = 0u;
            nofs_format_retry = 0u;
          } else {
            g_app.sd_state = SD_STATE_ERROR;
            g_app.sd_code = sd_fresult_to_code(fres);
          }
        } else {
          g_app.sd_state = SD_STATE_ERROR;
          g_app.sd_code = sd_fresult_to_code(fres);
        }
      } else {
        g_app.sd_state = SD_STATE_READY;
        g_app.sd_code = (uint16_t)FR_NO_FILESYSTEM;
      }
#else
      g_app.sd_state = SD_STATE_READY;
      g_app.sd_code = (uint16_t)fres;
#endif
    } else {
      g_app.sd_state = SD_STATE_ERROR;
      g_app.sd_code = sd_fresult_to_code(fres);
    }

    /* Nếu chưa mount thành công, delay rồi thử lại */
    if (g_app.sd_state != SD_STATE_MOUNTED) {
      osDelay(800);
    }
  }
  /* USER CODE END StartTaskSD */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
