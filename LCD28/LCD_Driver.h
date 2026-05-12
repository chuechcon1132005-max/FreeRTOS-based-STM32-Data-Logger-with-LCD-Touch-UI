/*****************************************************************************
* | File      	:		LCD_Driver.h
* | Author      :   Waveshare team
* | Function    :   LCD_Driver driver of ST7789 & HX8347
* | Info        :
*----------------
* |	This version:   V1.2
* | Date        :   2019-08-16
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#ifndef __LCD_H__
#define __LCD_H__

#include <stdint.h>
#include "main.h"

#ifndef UINT32_MAX
typedef unsigned int uint32_t;
#endif

#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

//color define
#define LCD_WIDTH    	240
#define LCD_HEIGHT   	320

#define FONT_1206    	12
#define FONT_1608    	16
#define FONT_GB2312  	16

#define WHITE          0xFFFF
#define BLACK          0x0000	  
#define BLUE           0x001F  
#define BRED           0XF81F
#define GRED 		   	   0XFFE0
#define GBLUE		   		 0X07FF
#define RED            0xF800
#define MAGENTA        0xF81F
#define GREEN          0x07E0
#define CYAN           0x7FFF
#define YELLOW         0xFFE0
#define BROWN 		   	 0XBC40 
#define BRRED 		   	 0XFC07 
#define GRAY  		   	 0X8430 


/* PB3(SCK) PA6(MISO) PA7(MOSI), control pins on GPIOB and TP_CS on GPIOA */
#define LCD_DC_H()     HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET)
#define LCD_DC_L()     HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET)

#define LCD_CS_H()     HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET)
#define LCD_CS_L()     HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET)

#define LCD_BKL_H()    HAL_GPIO_WritePin(LCD_BKL_GPIO_Port, LCD_BKL_Pin, GPIO_PIN_SET)
#define LCD_BKL_L()    HAL_GPIO_WritePin(LCD_BKL_GPIO_Port, LCD_BKL_Pin, GPIO_PIN_RESET)

#define LCD_RST_H()    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET)
#define LCD_RST_L()    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET)

#define LCD_CMD                0
#define LCD_DATA               1

#define ST7789_DEVICE
//#define HX8347D_DEVICE

void spi_init(void);
uint8_t spi1_communication(uint8_t send_char);
void lcd_init(void);
void lcd_set_cursor(uint16_t hwXpos, uint16_t hwYpos);
void lcd_display_GBchar(uint16_t hwXpos,uint16_t hwYpos,uint8_t chChr,uint8_t chSize,uint16_t hwColor);
void lcd_display_GB2312(uint8_t gb,uint16_t color_front,uint16_t postion_x,uint16_t postion_y );
void lcd_draw_dot(uint16_t hwXpos, uint16_t hwYpos, uint16_t hwColor);
void lcd_draw_bigdot(unsigned long color_front,unsigned long x,unsigned long y );
void lcd_display_number(unsigned long x,unsigned long y,unsigned long num,uint8_t num_len);
void lcd_clear_screen(uint16_t hwColor);
void lcd_draw_dot(uint16_t hwXpos, uint16_t hwYpos, uint16_t hwColor);
void lcd_display_char(uint16_t hwXpos,uint16_t hwYpos,uint8_t chChr,uint8_t chSize,uint16_t hwColor); 
void lcd_display_num(uint16_t hwXpos,uint16_t hwYpos,unsigned long chNum,uint8_t chLen,uint8_t chSize,uint16_t hwColor); 
void lcd_display_string(uint16_t hwXpos,uint16_t hwYpos,const uint8_t *pchString,uint8_t chSize,uint16_t hwColor); 
void lcd_draw_line(uint16_t hwXpos0,uint16_t hwYpos0,uint16_t hwXpos1,uint16_t hwYpos1,uint16_t hwColor);
void lcd_draw_circle(uint16_t hwXpos,uint16_t hwYpos,uint16_t hwRadius,uint16_t hwColor);
void lcd_fill_rect(uint16_t hwXpos,uint16_t hwYpos,uint16_t hwWidth,uint16_t hwHeight,uint16_t hwColor); 
void lcd_draw_v_line(uint16_t hwXpos,uint16_t hwYpos,uint16_t hwHeight,uint16_t hwColor);
void lcd_draw_h_line(uint16_t hwXpos,uint16_t hwYpos,uint16_t hwWidth,uint16_t hwColor);
void lcd_draw_rect(uint16_t hwXpos,uint16_t hwYpos,uint16_t hwWidth,uint16_t hwHeight,uint16_t hwColor); 
void lcd_clear_Rect(unsigned long color_front,unsigned long x0,unsigned long y0,unsigned long x1,unsigned long y1);
												
#endif
