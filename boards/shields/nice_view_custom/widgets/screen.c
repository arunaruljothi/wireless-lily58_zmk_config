/*
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Left-half status screen in the style of nice-view-gem
 * (https://github.com/M165437/nice-view-gem, MIT):
 *
 *   SIG  [icon]      connection: BLE connected / not connected / open, or USB
 *   BAT L     82%    this half
 *   BAT R     64%    right half ("--" until its first reading)
 *   [■][ ][ ][ ][ ]  BLE profiles 1-5, filled = active, X = active and cleared
 *   (crystal)        animates only while HYPER is held
 *   LAYER            active layer name
 *
 * Everything is rendered into a 68x160 portrait 1-bit framebuffer, then copied
 * into the three 68x68 canvases and rotated, the same way the stock nice!view
 * widget lays out its canvases.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/modifiers.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/display.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#include "screen.h"

/**
 * Framebuffer and drawing primitives (portrait coordinates)
 **/

#define SCREEN_W 68
#define SCREEN_H 160

#define PROFILE_COUNT 5
#define SQUARE_SIZE 12
#define SQUARE_GAP 2
#define SQUARES_Y 47
#define GEM_Y 64
#define LAYER_Y 140

#define FONT_LINE_HEIGHT 13
#define FONT_BASE_LINE 2
#define FONT_ADVANCE 8

#define PIXEL_FG (IS_ENABLED(CONFIG_NICE_VIEW_CUSTOM_WIDGET_INVERTED) ? 0xFF : 0x00)
#define PIXEL_BG (IS_ENABLED(CONFIG_NICE_VIEW_CUSTOM_WIDGET_INVERTED) ? 0x00 : 0xFF)

static uint8_t fb[SCREEN_W * SCREEN_H]; // 1 = foreground

static void set_px(int x, int y, bool on) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        fb[y * SCREEN_W + x] = on;
    }
}

static void fill_rect(int x, int y, int w, int h, bool on) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            set_px(x + i, y + j, on);
        }
    }
}

static void outline_rect(int x, int y, int w, int h) {
    fill_rect(x, y, w, 1, true);
    fill_rect(x, y + h - 1, w, 1, true);
    fill_rect(x, y, 1, h, true);
    fill_rect(x + w - 1, y, 1, h, true);
}

static void draw_line(int x0, int y0, int x1, int y1) {
    int n = MAX(MAX(abs(x1 - x0), abs(y1 - y0)), 1);
    for (int i = 0; i <= n; i++) {
        set_px(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, true);
    }
}

// 1-bit indexed (I1) images: bit set = foreground, rows padded to whole bytes.
static void draw_bitmap(const uint8_t *data, int w, int h, int x, int y) {
    int stride = (w + 7) / 8;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            set_px(x + c, y + r, (data[r * stride + (c >> 3)] >> (7 - (c & 7))) & 1);
        }
    }
}

/**
 * Text (Pixel Operator Mono, from nice-view-gem)
 **/

const uint8_t *pixel_operator_mono_bitmap(void);
const lv_font_fmt_txt_glyph_dsc_t *pixel_operator_mono_glyphs(void);

static void draw_glyph(char ch, int x, int y) {
    if (ch < 0x20 || ch > 0x7E) {
        return;
    }

    const lv_font_fmt_txt_glyph_dsc_t *g = &pixel_operator_mono_glyphs()[ch - 0x20 + 1];
    const uint8_t *bitmap = pixel_operator_mono_bitmap();
    int top = y + (FONT_LINE_HEIGHT - FONT_BASE_LINE) - g->box_h - g->ofs_y;
    uint32_t bit = g->bitmap_index * 8;

    for (int r = 0; r < g->box_h; r++) {
        for (int c = 0; c < g->box_w; c++, bit++) {
            if ((bitmap[bit >> 3] >> (7 - (bit & 7))) & 1) {
                set_px(x + g->ofs_x + c, top + r, true);
            }
        }
    }
}

enum text_align { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER };

static void draw_text(const char *text, int x, int y, int max_w, enum text_align align) {
    int w = strlen(text) * FONT_ADVANCE;
    if (align == ALIGN_RIGHT) {
        x += max_w - w;
    } else if (align == ALIGN_CENTER) {
        x += (max_w - w) / 2;
    }

    for (; *text; text++, x += FONT_ADVANCE) {
        draw_glyph(*text, x, y);
    }
}

/**
 * Icons and crystal art (from nice-view-gem)
 **/

static const uint8_t bolt_bits[] = {0x08, 0x10, 0x20, 0x40, 0xf8, 0x10, 0x20, 0x40, 0x80};

static const uint8_t bt_bits[] = {
    0xfb, 0xf0, 0xf9, 0xf0, 0xf8, 0xf0, 0xda, 0x70, 0xcb, 0x30, 0xe2, 0x70, 0xf0, 0xf0, 0xf9,
    0xf0, 0xf0, 0xf0, 0xe2, 0x70, 0xcb, 0x30, 0xda, 0x70, 0xf8, 0xf0, 0xf9, 0xf0, 0xfb, 0xf0,
};

static const uint8_t bt_no_signal_bits[] = {
    0xfb, 0xf0, 0x79, 0xf0, 0x38, 0xf0, 0x9a, 0x70, 0xcf, 0x30, 0xe6, 0x70, 0xf3, 0xf0, 0xf9,
    0xf0, 0xfc, 0xf0, 0xe6, 0x70, 0xcb, 0x30, 0xdb, 0x90, 0xf8, 0xc0, 0xf9, 0xe0, 0xfb, 0xf0,
};

static const uint8_t bt_unbonded_bits[] = {
    0x00, 0x20, 0x00, 0x00, 0x30, 0x00, 0x00, 0x38, 0x00, 0x21, 0x2c, 0x10, 0x41, 0xa6, 0x08,
    0x48, 0xec, 0x48, 0x90, 0x78, 0x24, 0x92, 0x31, 0x24, 0x90, 0x78, 0x24, 0x48, 0xec, 0x48,
    0x41, 0xa6, 0x08, 0x21, 0x2c, 0x10, 0x00, 0x38, 0x00, 0x00, 0x30, 0x00, 0x00, 0x20, 0x00,
};

static const uint8_t usb_bits[] = {
    0xff, 0x9f, 0xf0, 0xfe, 0x0f, 0xf0, 0xfc, 0x9f, 0xf0, 0xf9, 0xff, 0xb0, 0x1b,
    0xff, 0x90, 0x00, 0x00, 0x00, 0x1f, 0x7f, 0x90, 0xff, 0x3f, 0xb0, 0xff, 0x91,
    0xf0, 0xff, 0xc1, 0xf0, 0xff, 0xf1, 0xf0,
};

// Crystal frames are 69x68 landscape images; each map starts with an 8-byte palette.
#define CRYSTAL_W 69
#define CRYSTAL_H 68
#define CRYSTAL_PALETTE_SIZE 8
#define CRYSTAL_FRAMES 16

extern const uint8_t crystal_01_map[], crystal_02_map[], crystal_03_map[], crystal_04_map[],
    crystal_05_map[], crystal_06_map[], crystal_07_map[], crystal_08_map[], crystal_09_map[],
    crystal_10_map[], crystal_11_map[], crystal_12_map[], crystal_13_map[], crystal_14_map[],
    crystal_15_map[], crystal_16_map[];

static const uint8_t *const crystal_frames[CRYSTAL_FRAMES] = {
    crystal_01_map, crystal_02_map, crystal_03_map, crystal_04_map,
    crystal_05_map, crystal_06_map, crystal_07_map, crystal_08_map,
    crystal_09_map, crystal_10_map, crystal_11_map, crystal_12_map,
    crystal_13_map, crystal_14_map, crystal_15_map, crystal_16_map,
};

// Rotate 90° clockwise into the portrait framebuffer.
static void draw_crystal(int frame, int y) {
    const uint8_t *data = crystal_frames[frame] + CRYSTAL_PALETTE_SIZE;
    int stride = (CRYSTAL_W + 7) / 8;
    for (int r = 0; r < CRYSTAL_H; r++) {
        for (int c = 0; c < CRYSTAL_W; c++) {
            if ((data[r * stride + (c >> 3)] >> (7 - (c & 7))) & 1) {
                set_px(CRYSTAL_H - 1 - r, y + c, true);
            }
        }
    }
}

/**
 * Screen state
 **/

static struct {
    uint8_t battery;
    bool charging;
    uint8_t peripheral_battery;
    bool peripheral_battery_known;
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    const char *layer_label;
    uint8_t layer_index;
    int crystal_frame;
} state;

static struct zmk_widget_screen *screen_widget;
static lv_timer_t *animation_timer;

/**
 * Rendering
 **/

static void render_status(void) {
    char text[12];

    // SIG
    draw_text("SIG", 0, 1, 25, ALIGN_LEFT);
    fill_rect(43, 0, 24, 15, true);
    switch (state.selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        draw_bitmap(usb_bits, 20, 11, 45, 2);
        break;
    case ZMK_TRANSPORT_BLE:
        if (!state.active_profile_bonded) {
            draw_bitmap(bt_unbonded_bits, 22, 15, 44, 0);
        } else if (state.active_profile_connected) {
            draw_bitmap(bt_bits, 12, 15, 49, 0);
        } else {
            draw_bitmap(bt_no_signal_bits, 12, 15, 49, 0);
        }
        break;
    }

    // BAT L (a bolt replaces the % while charging)
    draw_text("BAT", 0, 16, 25, ALIGN_LEFT);
    draw_text("L", 28, 16, 8, ALIGN_LEFT);
    if (state.charging) {
        snprintf(text, sizeof(text), state.battery == 100 ? "%u" : "%u%%", state.battery);
        draw_text(text, 36, 16, 25, ALIGN_RIGHT);
        draw_bitmap(bolt_bits, 5, 9, 62, 18);
    } else {
        snprintf(text, sizeof(text), "%u%%", state.battery);
        draw_text(text, 36, 16, 32, ALIGN_RIGHT);
    }

    // BAT R
    draw_text("BAT", 0, 30, 25, ALIGN_LEFT);
    draw_text("R", 28, 30, 8, ALIGN_LEFT);
    if (state.peripheral_battery_known) {
        snprintf(text, sizeof(text), "%u%%", state.peripheral_battery);
    } else {
        strcpy(text, "--");
    }
    draw_text(text, 36, 30, 32, ALIGN_RIGHT);

    // BLE profiles
    for (int i = 0; i < PROFILE_COUNT; i++) {
        int x = i * (SQUARE_SIZE + SQUARE_GAP);
        int y = SQUARES_Y;
        bool active = i == state.active_profile_index;

        if (active && !state.active_profile_bonded) {
            outline_rect(x, y, SQUARE_SIZE, SQUARE_SIZE);
            draw_line(x + 2, y + 2, x + SQUARE_SIZE - 3, y + SQUARE_SIZE - 3);
            draw_line(x + SQUARE_SIZE - 3, y + 2, x + 2, y + SQUARE_SIZE - 3);
        } else if (active) {
            fill_rect(x, y, SQUARE_SIZE, SQUARE_SIZE, true);
        } else {
            outline_rect(x, y, SQUARE_SIZE, SQUARE_SIZE);
        }
    }

    // Layer
    if (state.layer_label != NULL && strlen(state.layer_label) > 0) {
        strncpy(text, state.layer_label, 8);
        text[8] = '\0';
        for (char *p = text; *p; p++) {
            *p = toupper((unsigned char)*p);
        }
    } else {
        snprintf(text, sizeof(text), "LAYER %u", state.layer_index);
    }
    draw_text(text, 0, LAYER_Y, SCREEN_W, ALIGN_CENTER);
}

static void rotate_canvas(lv_obj_t *canvas) {
    uint8_t *buf = lv_canvas_get_draw_buf(canvas)->data;

    static uint8_t buf_copy[CANVAS_BUF_SIZE];
    memcpy(buf_copy, buf, sizeof(buf_copy));

    const uint32_t stride = lv_draw_buf_width_to_stride(CANVAS_SIZE, CANVAS_COLOR_FORMAT);

#if IS_ENABLED(CONFIG_NICE_VIEW_CUSTOM_ROTATE_180)
    lv_draw_sw_rotate(buf_copy, buf, CANVAS_SIZE, CANVAS_SIZE, stride, stride,
                      LV_DISPLAY_ROTATION_90, CANVAS_COLOR_FORMAT);
#else
    lv_draw_sw_rotate(buf_copy, buf, CANVAS_SIZE, CANVAS_SIZE, stride, stride,
                      LV_DISPLAY_ROTATION_270, CANVAS_COLOR_FORMAT);
#endif
}

// Copy framebuffer rows [index * 68, index * 68 + 68) into canvas `index`.
static void blit_canvas(lv_obj_t *canvas, int index) {
    uint8_t *buf = lv_canvas_get_draw_buf(canvas)->data;
    const uint32_t stride = lv_draw_buf_width_to_stride(CANVAS_SIZE, CANVAS_COLOR_FORMAT);

    for (int y = 0; y < CANVAS_SIZE; y++) {
        int fb_y = index * CANVAS_SIZE + y;
        for (int x = 0; x < CANVAS_SIZE; x++) {
            bool on = fb_y < SCREEN_H && fb[fb_y * SCREEN_W + x];
            buf[y * stride + x] = on ? PIXEL_FG : PIXEL_BG;
        }
    }

    rotate_canvas(canvas);
    lv_obj_invalidate(canvas);
}

static void redraw(void) {
    if (screen_widget == NULL) {
        return;
    }

    memset(fb, 0, sizeof(fb));
    render_status();
    draw_crystal(state.crystal_frame, GEM_Y);

    for (int i = 0; i < 3; i++) {
        blit_canvas(lv_obj_get_child(screen_widget->obj, i), i);
    }
}

static void animation_tick(lv_timer_t *timer) {
    state.crystal_frame = (state.crystal_frame + 1) % CRYSTAL_FRAMES;
    redraw();
}

/**
 * Battery status
 **/

struct battery_status_state {
    uint8_t level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool usb_present;
#endif
};

static void battery_status_update_cb(struct battery_status_state s) {
    state.battery = s.level;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    state.charging = s.usb_present;
#endif
    redraw();
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev =
        (eh != NULL) ? as_zmk_battery_state_changed(eh) : NULL;

    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

/**
 * Peripheral (right half) battery status
 **/

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
struct peripheral_battery_status_state {
    uint8_t level;
    bool known;
};

static void peripheral_battery_status_update_cb(struct peripheral_battery_status_state s) {
    state.peripheral_battery = s.level;
    state.peripheral_battery_known = s.known;
    redraw();
}

static struct peripheral_battery_status_state
peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        (eh != NULL) ? as_zmk_peripheral_battery_state_changed(eh) : NULL;

    if (ev != NULL) {
        return (struct peripheral_battery_status_state){.level = ev->state_of_charge,
                                                        .known = true};
    }

    uint8_t level = 0;
    bool known = zmk_split_central_get_peripheral_battery_level(0, &level) == 0 && level > 0;
    return (struct peripheral_battery_status_state){.level = level, .known = known};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery_status,
                            struct peripheral_battery_status_state,
                            peripheral_battery_status_update_cb,
                            peripheral_battery_status_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_battery_status, zmk_peripheral_battery_state_changed);
#endif

/**
 * Output status
 **/

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

static void output_status_update_cb(struct output_status_state s) {
    state.selected_endpoint = s.selected_endpoint;
    state.active_profile_index = s.active_profile_index;
    state.active_profile_connected = s.active_profile_connected;
    state.active_profile_bonded = s.active_profile_bonded;
    redraw();
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoint_get_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/**
 * Layer status
 **/

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void layer_status_update_cb(struct layer_status_state s) {
    state.layer_index = s.index;
    state.layer_label = s.label;
    redraw();
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/**
 * HYPER (LS(LC(LA(LGUI)))) held: animate the crystal
 **/

#define HYPER_MODS (MOD_LCTL | MOD_LSFT | MOD_LALT)

struct hyper_status_state {
    bool held;
};

static bool hyper_held;

static void hyper_status_update_cb(struct hyper_status_state s) {
    if (animation_timer == NULL) {
        return;
    }

    if (s.held) {
        lv_timer_resume(animation_timer);
    } else {
        lv_timer_pause(animation_timer);
    }
}

static struct hyper_status_state hyper_status_get_state(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev =
        (eh != NULL) ? as_zmk_keycode_state_changed(eh) : NULL;

    if (ev != NULL && ev->usage_page == HID_USAGE_KEY &&
        ev->keycode == HID_USAGE_KEY_KEYBOARD_LEFT_GUI &&
        ((ev->implicit_modifiers | ev->explicit_modifiers) & HYPER_MODS) == HYPER_MODS) {
        hyper_held = ev->state;
    }

    return (struct hyper_status_state){.held = hyper_held};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_hyper_status, struct hyper_status_state, hyper_status_update_cb,
                            hyper_status_get_state)
ZMK_SUBSCRIPTION(widget_hyper_status, zmk_keycode_state_changed);

/**
 * Initialization
 **/

#if IS_ENABLED(CONFIG_NICE_VIEW_CUSTOM_ROTATE_180) // positions for default and flipped canvases
static const int canvas_pos[3] = {0, 68, 136};
#else
static const int canvas_pos[3] = {92, 24, -44};
#endif

int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    uint8_t *bufs[3] = {widget->cbuf, widget->cbuf2, widget->cbuf3};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *canvas = lv_canvas_create(widget->obj);
        lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, canvas_pos[i], 0);
        lv_canvas_set_buffer(canvas, bufs[i], CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);
    }

    animation_timer = lv_timer_create(animation_tick,
                                      CONFIG_NICE_VIEW_CUSTOM_ANIMATION_MS / CRYSTAL_FRAMES, NULL);
    lv_timer_pause(animation_timer);

    widget_battery_status_init();
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    widget_peripheral_battery_status_init();
#endif
    widget_output_status_init();
    widget_layer_status_init();
    widget_hyper_status_init();

    screen_widget = widget;
    redraw();

    return 0;
}

lv_obj_t *zmk_widget_screen_obj(struct zmk_widget_screen *widget) { return widget->obj; }
