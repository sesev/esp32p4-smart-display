/**
 * screen_manager.c  (LVGL 9.5+)
 *
 * Sleep / wake / analog-clock state machine.
 *
 * States:
 *   SCR_ACTIVE  – dashboard visible, backlight 100 %
 *   SCR_SLEEP   – backlight off, separate black screen loaded
 *   SCR_CLOCK   – full-screen blur overlay on dashboard + analog clock
 *
 * Transitions:
 *   ACTIVE → SLEEP  : inactivity >= sleep_min
 *   SLEEP  → CLOCK  : any tap (backlight on, overlay created, dashboard loaded)
 *   CLOCK  → ACTIVE : any tap (overlay fades out + deleted, dashboard revealed)
 *
 * Clock is a child overlay of s_main_scr, NOT a separate screen.
 * This lets LVGL 9.5 blur_backdrop see the dashboard tiles behind it —
 * giving a frosted-glass effect at zero extra cost.
 *
 * All LVGL calls run inside timer / event callbacks — no bsp_display_lock needed.
 */

#include "screen_manager.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "scr_mgr";

/* ── State machine ─────────────────────────────────────────────────────── */

typedef enum { SCR_ACTIVE, SCR_SLEEP, SCR_CLOCK } scr_state_t;

static scr_state_t       s_state    = SCR_ACTIVE;
static uint32_t          s_sleep_ms = 0;
static screen_wake_cb_t  s_wake_cb  = NULL;

static lv_obj_t   *s_main_scr      = NULL;  /* saved dashboard screen        */
static lv_obj_t   *s_sleep_scr     = NULL;  /* separate black screen         */
static lv_obj_t   *s_clock_overlay = NULL;  /* full-screen child of main_scr */

static lv_timer_t *s_idle_tmr      = NULL;  /* 10 s inactivity poll          */
static lv_timer_t *s_clock_tmr     = NULL;  /* 1 s hand update               */

/* ── Clock geometry ────────────────────────────────────────────────────── */

#define CLK_CX   400    /* center X (display 800 px wide)    */
#define CLK_CY   560    /* center Y — upper half of 1280     */
#define CLK_R    280    /* clock face radius, px             */

/* Static point arrays (LVGL stores pointer, not a copy) */
static lv_point_precise_t s_tick_pts[60][2];
static lv_point_precise_t s_hour_pts[2];
static lv_point_precise_t s_min_pts[2];
static lv_point_precise_t s_sec_pts[2];

/* Handles to clock hands (valid only in SCR_CLOCK) */
static lv_obj_t *s_hour_line = NULL;
static lv_obj_t *s_min_line  = NULL;
static lv_obj_t *s_sec_line  = NULL;
static lv_obj_t *s_time_lbl  = NULL;

/* ── Geometry helpers ──────────────────────────────────────────────────── */

static inline lv_point_precise_t hand_tip(float angle_deg, int len)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    return (lv_point_precise_t){
        .x = (lv_value_precise_t)(CLK_CX + len * sinf(rad)),
        .y = (lv_value_precise_t)(CLK_CY - len * cosf(rad)),
    };
}

/* ── Update clock hands ────────────────────────────────────────────────── */

static void update_hands(void)
{
    time_t now   = time(NULL);
    bool   valid = (now > 1700000000UL);   /* only true after SNTP sync */

    struct tm t;
    if (valid) {
        localtime_r(&now, &t);
    } else {
        memset(&t, 0, sizeof(t));
        t.tm_hour = 12;
    }

    float h_ang = (t.tm_hour % 12) * 30.0f + t.tm_min * 0.5f;
    float m_ang =  t.tm_min  * 6.0f         + t.tm_sec * 0.1f;
    float s_ang =  t.tm_sec  * 6.0f;

    s_hour_pts[0] = (lv_point_precise_t){ CLK_CX, CLK_CY };
    s_hour_pts[1] = hand_tip(h_ang, 140);
    lv_line_set_points(s_hour_line, s_hour_pts, 2);

    s_min_pts[0] = (lv_point_precise_t){ CLK_CX, CLK_CY };
    s_min_pts[1] = hand_tip(m_ang, 200);
    lv_line_set_points(s_min_line, s_min_pts, 2);

    s_sec_pts[0] = (lv_point_precise_t){ CLK_CX, CLK_CY };
    s_sec_pts[1] = hand_tip(s_ang, 220);
    lv_line_set_points(s_sec_line, s_sec_pts, 2);

    char tbuf[9];
    snprintf(tbuf, sizeof(tbuf), valid ? "%02d:%02d" : "--:--",
             t.tm_hour, t.tm_min);
    lv_label_set_text(s_time_lbl, tbuf);
}

/* ── Clock timer (1 s) ─────────────────────────────────────────────────── */

static void clock_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (s_state == SCR_CLOCK && s_hour_line) update_hands();
}

/* ── Clock overlay: tap → back to dashboard ─────────────────────────────── */

static void clock_overlay_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Clock tapped — back to dashboard");

    s_state = SCR_ACTIVE;

    if (s_clock_tmr) {
        lv_timer_delete(s_clock_tmr);
        s_clock_tmr = NULL;
    }

    /* Fade overlay out then delete it — dashboard underneath is revealed */
    lv_obj_fade_out(s_clock_overlay, 300, 0);
    lv_obj_delete_delayed(s_clock_overlay, 350);
    s_clock_overlay = NULL;
    s_hour_line = s_min_line = s_sec_line = s_time_lbl = NULL;

    if (s_wake_cb) s_wake_cb();
}

/* ── Build the clock overlay (child of s_main_scr) ─────────────────────── */

static void build_clock_overlay(void)
{
    lv_obj_t *ov = lv_obj_create(s_main_scr);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ov, 800, 1280);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);

    /* Dark tint + blur backdrop — LVGL 9.5 frosted-glass */
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x050a0f), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_80, 0);
    lv_obj_set_style_blur_backdrop(ov, true, 0);
    lv_obj_set_style_blur_radius(ov, 12, 0);
    lv_obj_set_style_blur_quality(ov, LV_BLUR_QUALITY_SPEED, 0);

    lv_obj_add_event_cb(ov, clock_overlay_event_cb, LV_EVENT_CLICKED, NULL);

    /* Clock face ring */
    lv_obj_t *face = lv_obj_create(ov);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(face, CLK_R * 2, CLK_R * 2);
    lv_obj_set_pos(face, CLK_CX - CLK_R, CLK_CY - CLK_R);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(face, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(face, 2, 0);
    lv_obj_set_style_pad_all(face, 0, 0);

    /* Tick marks */
    for (int i = 0; i < 60; i++) {
        bool  is_hour = (i % 5 == 0);
        float ang     = i * 6.0f * (float)M_PI / 180.0f;
        int r_outer   = CLK_R - 2;
        int r_inner   = is_hour ? (CLK_R - 24) : (CLK_R - 10);

        s_tick_pts[i][0].x = (lv_value_precise_t)(CLK_CX + r_outer * sinf(ang));
        s_tick_pts[i][0].y = (lv_value_precise_t)(CLK_CY - r_outer * cosf(ang));
        s_tick_pts[i][1].x = (lv_value_precise_t)(CLK_CX + r_inner * sinf(ang));
        s_tick_pts[i][1].y = (lv_value_precise_t)(CLK_CY - r_inner * cosf(ang));

        lv_obj_t *tick = lv_line_create(ov);
        lv_obj_set_pos(tick, 0, 0);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
        lv_line_set_points(tick, s_tick_pts[i], 2);
        lv_obj_set_style_line_width(tick, is_hour ? 3 : 1, 0);
        lv_obj_set_style_line_color(tick,
            lv_color_hex(is_hour ? 0x8b949e : 0x3d444d), 0);
    }

    /* Hour hand — thick white */
    s_hour_line = lv_line_create(ov);
    lv_obj_set_pos(s_hour_line, 0, 0);
    lv_obj_remove_flag(s_hour_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_hour_line, 8, 0);
    lv_obj_set_style_line_color(s_hour_line, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_line_rounded(s_hour_line, true, 0);

    /* Minute hand — medium white */
    s_min_line = lv_line_create(ov);
    lv_obj_set_pos(s_min_line, 0, 0);
    lv_obj_remove_flag(s_min_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_min_line, 5, 0);
    lv_obj_set_style_line_color(s_min_line, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_style_line_rounded(s_min_line, true, 0);

    /* Second hand — thin teal */
    s_sec_line = lv_line_create(ov);
    lv_obj_set_pos(s_sec_line, 0, 0);
    lv_obj_remove_flag(s_sec_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(s_sec_line, 2, 0);
    lv_obj_set_style_line_color(s_sec_line, lv_color_hex(0x40c080), 0);
    lv_obj_set_style_line_rounded(s_sec_line, true, 0);

    /* Center pivot dot */
    lv_obj_t *dot = lv_obj_create(ov);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_set_pos(dot, CLK_CX - 8, CLK_CY - 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x40c080), 0);
    lv_obj_set_style_border_width(dot, 0, 0);

    /* Digital HH:MM below the face */
    s_time_lbl = lv_label_create(ov);
    lv_obj_remove_flag(s_time_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(0xe6edf3), 0);
    lv_obj_set_width(s_time_lbl, 200);
    lv_obj_set_style_text_align(s_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_time_lbl, CLK_CX - 100, CLK_CY + CLK_R + 28);
    lv_label_set_text(s_time_lbl, "--:--");

    /* Hint text */
    lv_obj_t *hint = lv_label_create(ov);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3d444d), 0);
    lv_obj_set_width(hint, 200);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, CLK_CX - 100, 1200);
    lv_label_set_text(hint, "Tap to go home");

    s_clock_overlay = ov;
    update_hands();
}

/* ── Sleep screen: tap → wake + clock ──────────────────────────────────── */

static void sleep_scr_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Wake from sleep — showing clock");

    bsp_display_brightness_set(100);

    /* Build clock overlay on the dashboard, then fade the dashboard in.
     * The overlay is already a child of s_main_scr, so it appears together. */
    build_clock_overlay();
    lv_screen_load_anim(s_main_scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);

    lv_obj_delete_async(s_sleep_scr);
    s_sleep_scr = NULL;

    s_state = SCR_CLOCK;
    s_clock_tmr = lv_timer_create(clock_timer_cb, 1000, NULL);
}

/* ── Go to sleep ────────────────────────────────────────────────────────── */

static void go_sleep(void)
{
    ESP_LOGI(TAG, "Inactivity timeout — display off");

    /* If clock overlay is somehow still alive, kill it first */
    if (s_clock_overlay) {
        if (s_clock_tmr) { lv_timer_delete(s_clock_tmr); s_clock_tmr = NULL; }
        lv_obj_delete(s_clock_overlay);
        s_clock_overlay = NULL;
        s_hour_line = s_min_line = s_sec_line = s_time_lbl = NULL;
    }

    s_sleep_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_sleep_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_sleep_scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_sleep_scr, sleep_scr_event_cb, LV_EVENT_CLICKED, NULL);

    lv_screen_load_anim(s_sleep_scr, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);

    s_state = SCR_SLEEP;
    bsp_display_brightness_set(0);
}

/* ── Idle check timer (10 s) ────────────────────────────────────────────── */

static void idle_timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    if (s_state != SCR_ACTIVE) return;
    if (lv_display_get_inactive_time(NULL) >= s_sleep_ms) go_sleep();
}

/* ── Public API ─────────────────────────────────────────────────────────── */

lv_obj_t *screen_manager_get_dashboard(void)
{
    return s_main_scr;
}

void screen_manager_init(uint32_t sleep_min, screen_wake_cb_t wake_cb)
{
    s_sleep_ms = sleep_min * 60u * 1000u;
    s_wake_cb  = wake_cb;
    s_state    = SCR_ACTIVE;
    s_main_scr = lv_screen_active();

    s_idle_tmr = lv_timer_create(idle_timer_cb, 10000, NULL);

    ESP_LOGI(TAG, "init: sleep after %lu min", (unsigned long)sleep_min);
}
