/* 
 * badapple_waveshare.c - Bad Apple video player for Waveshare 1.28" SPI display
 * 
 * Expands the original badapple.c to support GC9A01A 240x240 RGB565 SPI display
 * 
 * Author  : Martin Bogomolni
 * Date    : 2026-04-20
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 * 
 * Original badapple.c attribution:
 * - Music Rights: ZUN (Japan) holds the original song rights
 * - Shadow Video: The iconic 2009 black-and-white shadow-art video by Anira
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <zlib.h>

#include "lcd_gc9a01.h"
#include "gpio_config.h"

#define FRAME_WIDTH 240
#define FRAME_HEIGHT 240
#define FRAME_SIZE_BYTES (FRAME_WIDTH * FRAME_HEIGHT / 8)  /* 1-bit monochrome */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <video_file> [fps]\n"
            "  video_file  - Path to 1-bit monochrome video (7200 bytes per frame)\n"
            "  fps         - Frames per second (default: 30)\n"
            "\n"
            "Plays Bad Apple video on Waveshare 1.28\" display (GC9A01A)\n"
            "\n"
            "Video format: 240x240 pixels, 1-bit monochrome (black=0, white=1)\n"
            "Frame size: 7200 bytes per frame\n",
            argv0);
}

/* Convert monochrome frame to RGB565 and display */
static void display_mono_frame(const uint8_t *mono_frame) {
    if (!mono_frame) return;
    
    /* Set address window for full screen */
    lcd_gc9a01a_set_address_window(0, 0, FRAME_WIDTH - 1, FRAME_HEIGHT - 1);
    
    /* Allocate temporary RGB565 frame buffer */
    uint16_t *rgb_frame = malloc(FRAME_WIDTH * FRAME_HEIGHT * sizeof(uint16_t));
    if (!rgb_frame) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        return;
    }
    
    /* Convert monochrome to RGB565 */
    size_t pixel_idx = 0;
    for (size_t byte_idx = 0; byte_idx < FRAME_SIZE_BYTES; byte_idx++) {
        uint8_t byte_val = mono_frame[byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            uint16_t color;
            if ((byte_val >> (7 - bit)) & 1) {
                color = 0xFFFF;  /* White */
            } else {
                color = 0x0000;  /* Black */
            }
            rgb_frame[pixel_idx++] = color;
        }
    }
    
    /* Draw the frame */
    lcd_gc9a01a_push_frame_buffer(rgb_frame);
    
    free(rgb_frame);
}

/* Play video from file */
static int play_video(const char *filename, int fps) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        perror("Failed to open video file");
        return -1;
    }
    
    printf("Playing: %s at %d FPS\n", filename, fps);
    printf("Frame size: %u bytes\n", (unsigned int)FRAME_SIZE_BYTES);
    
    uint8_t *frame_buffer = malloc(FRAME_SIZE_BYTES);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        fclose(fp);
        return -1;
    }
    
    struct timespec frame_delay;
    frame_delay.tv_sec = 0;
    frame_delay.tv_nsec = (1000000000LL / fps);  /* nanoseconds */
    
    int frame_count = 0;
    while (1) {
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        
        /* Read frame */
        size_t bytes_read = fread(frame_buffer, 1, FRAME_SIZE_BYTES, fp);
        if (bytes_read == 0) {
            break;  /* End of file */
        }
        
        if (bytes_read < FRAME_SIZE_BYTES) {
            /* Pad last frame with zeros */
            memset(frame_buffer + bytes_read, 0, FRAME_SIZE_BYTES - bytes_read);
        }
        
        /* Display frame */
        display_mono_frame(frame_buffer);
        
        frame_count++;
        if (frame_count % 30 == 0) {
            printf("Frame %d\n", frame_count);
        }
        
        /* Frame rate control */
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long elapsed_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000LL +
                         (end_time.tv_nsec - start_time.tv_nsec);
        
        if (elapsed_ns < frame_delay.tv_nsec) {
            struct timespec sleep_time;
            sleep_time.tv_sec = 0;
            sleep_time.tv_nsec = frame_delay.tv_nsec - elapsed_ns;
            nanosleep(&sleep_time, NULL);
        }
    }
    
    printf("Playback complete: %d frames played\n", frame_count);
    
    free(frame_buffer);
    fclose(fp);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    
    const char *video_file = argv[1];
    int fps = 30;
    
    if (argc > 2) {
        fps = atoi(argv[2]);
        if (fps < 1 || fps > 60) {
            fprintf(stderr, "Invalid FPS: %d (must be 1-60)\n", fps);
            return 1;
        }
    }
    
    printf("Initializing display...\n");
    if (lcd_gc9a01a_init() < 0) {
        fprintf(stderr, "Failed to initialize display\n");
        return 1;
    }
    
    printf("Display initialized\n");
    
    /* Play video */
    int ret = play_video(video_file, fps);
    
    printf("Cleaning up...\n");
    lcd_gc9a01a_deinit();
    
    return ret;
}
