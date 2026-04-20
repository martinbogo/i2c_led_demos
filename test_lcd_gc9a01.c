/*
 * test_lcd_gc9a01.c - Simple test program for Waveshare 1.28" display
 */

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include "lcd_gc9a01.h"

static volatile int running = 1;

void signal_handler(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    printf("Waveshare 1.28\" OLED Display Test\n");
    printf("==================================\n\n");
    
    signal(SIGINT, signal_handler);
    
    printf("Initializing display...\n");
    if (lcd_gc9a01a_init() < 0) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }
    printf("Display initialized successfully\n\n");
    
    /* Test 1: Fill screen with colors */
    printf("Test 1: Fill screen colors\n");
    
    printf("  - Black...\n");
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    sleep(1);
    
    printf("  - Red...\n");
    lcd_gc9a01a_fill_screen(COLOR_RED);
    sleep(1);
    
    printf("  - Green...\n");
    lcd_gc9a01a_fill_screen(COLOR_GREEN);
    sleep(1);
    
    printf("  - Blue...\n");
    lcd_gc9a01a_fill_screen(COLOR_BLUE);
    sleep(1);
    
    printf("  - White...\n");
    lcd_gc9a01a_fill_screen(COLOR_WHITE);
    sleep(1);
    
    /* Test 2: Fill screen black and draw rectangles */
    printf("Test 2: Draw rectangles\n");
    
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    sleep(1);
    
    printf("  - Draw red rectangle...\n");
    lcd_gc9a01a_draw_rect(10, 10, 100, 100, COLOR_RED);
    sleep(1);
    
    printf("  - Fill green rectangle...\n");
    lcd_gc9a01a_fill_rect(120, 20, 220, 120, COLOR_GREEN);
    sleep(1);
    
    printf("  - Draw blue line...\n");
    lcd_gc9a01a_draw_h_line(50, 150, 150, COLOR_BLUE);
    sleep(1);
    
    /* Test 3: Draw grid pattern */
    printf("Test 3: Draw grid pattern\n");
    
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    
    /* Horizontal lines */
    for (int i = 0; i < LCD_HEIGHT; i += 30) {
        lcd_gc9a01a_draw_h_line(0, i, LCD_WIDTH, COLOR_GRAY);
    }
    
    /* Vertical lines */
    for (int i = 0; i < LCD_WIDTH; i += 30) {
        lcd_gc9a01a_draw_v_line(i, 0, LCD_HEIGHT, COLOR_GRAY);
    }
    
    sleep(2);
    
    /* Test 4: Corner pixels */
    printf("Test 4: Draw corner pixels\n");
    
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    
    /* Draw corners */
    for (int i = 0; i < 20; i++) {
        lcd_gc9a01a_draw_pixel(i, i, COLOR_RED);                              /* Top-left */
        lcd_gc9a01a_draw_pixel(LCD_WIDTH - 1 - i, i, COLOR_GREEN);            /* Top-right */
        lcd_gc9a01a_draw_pixel(i, LCD_HEIGHT - 1 - i, COLOR_BLUE);            /* Bottom-left */
        lcd_gc9a01a_draw_pixel(LCD_WIDTH - 1 - i, LCD_HEIGHT - 1 - i, COLOR_YELLOW); /* Bottom-right */
    }
    
    sleep(2);
    
    /* Test 5: Rotation test */
    printf("Test 5: Rotation test\n");
    
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    lcd_gc9a01a_fill_rect(10, 10, 100, 100, COLOR_RED);
    lcd_gc9a01a_draw_rect(120, 120, 230, 230, COLOR_CYAN);
    sleep(1);
    
    printf("  - Rotating 90 degrees...\n");
    lcd_gc9a01a_set_rotation(GC9A01A_ROTATE_90);
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    lcd_gc9a01a_fill_rect(10, 10, 100, 100, COLOR_RED);
    lcd_gc9a01a_draw_rect(120, 120, 230, 230, COLOR_CYAN);
    sleep(1);
    
    printf("  - Rotating 180 degrees...\n");
    lcd_gc9a01a_set_rotation(GC9A01A_ROTATE_180);
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    lcd_gc9a01a_fill_rect(10, 10, 100, 100, COLOR_RED);
    lcd_gc9a01a_draw_rect(120, 120, 230, 230, COLOR_CYAN);
    sleep(1);
    
    printf("  - Rotating 270 degrees...\n");
    lcd_gc9a01a_set_rotation(GC9A01A_ROTATE_270);
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    lcd_gc9a01a_fill_rect(10, 10, 100, 100, COLOR_RED);
    lcd_gc9a01a_draw_rect(120, 120, 230, 230, COLOR_CYAN);
    sleep(1);
    
    /* Reset to 0 degrees */
    lcd_gc9a01a_set_rotation(GC9A01A_ROTATE_0);
    
    /* Test 6: Backlight control */
    printf("Test 6: Backlight control\n");
    
    lcd_gc9a01a_fill_screen(COLOR_CYAN);
    
    printf("  - Backlight off...\n");
    lcd_gc9a01a_set_backlight(0);
    sleep(1);
    
    printf("  - Backlight on...\n");
    lcd_gc9a01a_set_backlight(255);
    sleep(1);
    
    /* Final display */
    printf("Test 7: Final display\n");
    
    lcd_gc9a01a_fill_screen(COLOR_BLACK);
    
    /* Color gradient test */
    for (int x = 0; x < LCD_WIDTH; x += 5) {
        uint8_t r = (x * 255) / LCD_WIDTH;
        uint8_t g = ((LCD_WIDTH - x) * 255) / LCD_WIDTH;
        uint8_t b = 128;
        rgb565_t color = RGB565(r, g, b);
        lcd_gc9a01a_draw_v_line(x, 0, LCD_HEIGHT / 2, color);
    }
    
    sleep(2);
    
    printf("\n");
    printf("All tests completed successfully!\n");
    printf("Display is ready for video streaming.\n\n");
    
    lcd_gc9a01a_deinit();
    
    return 0;
}
