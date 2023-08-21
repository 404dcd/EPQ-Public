#include "window.h"
#include "channel.h"
#include "main.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int SCALE = 6;

Display* display;
Window window;
XRectangle* rectbuf;  // areas of screen to draw
int bufptr;
pthread_mutex_t rect_lock = PTHREAD_MUTEX_INITIALIZER;

int blank = 1;

void* window_listen_out(void* args);

void* run_window(void* args) {
    pthread_detach(pthread_self());
    rectbuf = malloc(32768 * sizeof(XRectangle));

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        pthread_exit(NULL);
    }

    // set up the screen
    int screen = DefaultScreen(display);
    GC gc = DefaultGC(display, screen);
    Window parent_window = DefaultRootWindow(display);
    unsigned int border_color = BlackPixel(display, screen);
    unsigned int background_color = BlackPixel(display, screen);
    window = XCreateSimpleWindow(display, parent_window, 0, 0, SCALE * 256, SCALE * 128, 0, border_color, background_color);
    XSetForeground(display, gc, WhitePixel(display, screen));

    uint32_t events = KeyPressMask | ExposureMask;
    XSelectInput(display, window, events);
    XMapWindow(display, window);
    XStoreName(display, window, "Emulator Screen");

    // run a listener for OUT instructions to the display
    pthread_t listen_thread;
    pthread_create(&listen_thread, NULL, window_listen_out, NULL);

    while (1) {
        XEvent event;
        // block until next X event
        XNextEvent(display, &event);

        if (event.type == KeyPress) {
            // send the data ready for an IN instruction, and raise interrupt
            channel_push(from_kbd, event.xkey.keycode);
            channel_push(ints_to_main, 0x10);
        }

        // either new frame or we need to redraw anyway
        if (event.type == Expose) {
            XClearWindow(display, window);
            if (!blank) {
                pthread_mutex_lock(&rect_lock);
                XFillRectangles(display, window, gc, rectbuf, bufptr);
                pthread_mutex_unlock(&rect_lock);
            }
        }
    }
    XCloseDisplay(display);
    exit(EXIT_SUCCESS);
}

void* window_listen_out(void* args) {
    XEvent exppp;
    pthread_detach(pthread_self());
    uint8_t* frame = malloc(4096);

    while (1) {
        // block until next OUT instruction
        uint32_t msg_to_screen = channel_pop_wait(to_screen);
        // memcpy -> DMA as quickly as possible, get out the way of the main thread
        memcpy(frame, mem + msg_to_screen, 4096);

        pthread_mutex_lock(&rect_lock);
        bufptr = 0;
        for (int y = 0; y < 128; y++) {
            for (int x = 0; x < 256; x++) {
                uint8_t mask = 1 << (7 - (x % 8));  // bitfield operations
                if (frame[y * 32 + (x / 8)] & mask) {
                    rectbuf[bufptr].x = SCALE * x;
                    rectbuf[bufptr].y = SCALE * y;
                    rectbuf[bufptr].width = SCALE;
                    rectbuf[bufptr].height = SCALE;
                    bufptr += 1;
                }
            }
        }
        pthread_mutex_unlock(&rect_lock);

        // make X event to signal redraw
        memset(&exppp, 0, sizeof(exppp));
        exppp.type = Expose;
        exppp.xexpose.window = window;
        blank = 0;  // keep screen blank until our first OUT instruction
        XSendEvent(display, window, False, ExposureMask, &exppp);
        XFlush(display);
    }
}