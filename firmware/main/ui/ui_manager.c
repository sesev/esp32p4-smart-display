#include "ui_manager.h"
#include "ha/ha_client.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FONT_16  (&lv_font_montserrat_16)
#define FONT_20  (&lv_font_montserrat_20)
#define FONT_24  (&lv_font_montserrat_24)

/* ── Colour palette ─────────────────────────────────────────────── */
#define C_BG      0x0d1117
#define C_CARD    0x161b22
#define C_BORDER  0x30363d
#define C_TEXT    0xe6edf3
#define C_MUTED   0x8b949e
#define C_GREEN   0x40c080
#define C_YELLOW  0xf0c040
#define C_BLUE    0x40b0f0
#define C_DIM     0x2a2f38
#define C_RED     0xe05050

#define SCR_W     800
#define NAV_H      88
#define CONTENT_H (1280 - NAV_H)   /* 1192 */

/* ── Icon name → LVGL symbol ─────────────────────────────────────── */
static const char *icon_sym(const char *name)
{
    if (!name)                         return LV_SYMBOL_SETTINGS;
    if (strcmp(name,"charge")   == 0)  return LV_SYMBOL_CHARGE;
    if (strcmp(name,"loop")     == 0)  return LV_SYMBOL_LOOP;
    if (strcmp(name,"ok")       == 0)  return LV_SYMBOL_OK;
    if (strcmp(name,"play")     == 0)  return LV_SYMBOL_PLAY;
    if (strcmp(name,"home")     == 0)  return LV_SYMBOL_HOME;
    if (strcmp(name,"bell")     == 0)  return LV_SYMBOL_BELL;
    if (strcmp(name,"wifi")     == 0)  return LV_SYMBOL_WIFI;
    if (strcmp(name,"settings") == 0)  return LV_SYMBOL_SETTINGS;
    if (strcmp(name,"power")    == 0)  return LV_SYMBOL_POWER;
    if (strcmp(name,"edit")     == 0)  return LV_SYMBOL_EDIT;
    if (strcmp(name,"list")     == 0)  return LV_SYMBOL_LIST;
    if (strcmp(name,"gps")      == 0)  return LV_SYMBOL_GPS;
    if (strcmp(name,"image")    == 0)  return LV_SYMBOL_IMAGE;
    if (strcmp(name,"audio")    == 0)  return LV_SYMBOL_AUDIO;
    return LV_SYMBOL_SETTINGS;
}

static const char *condition_str(const char *c)
{
    if (!c)                                   return "?";
    if (strcmp(c,"sunny")            == 0)    return "Aurinkoinen";
    if (strcmp(c,"clear-night")      == 0)    return "Kirkas yo";
    if (strcmp(c,"partlycloudy")     == 0)    return "Puolipilvista";
    if (strcmp(c,"cloudy")           == 0)    return "Pilvista";
    if (strcmp(c,"rainy")            == 0)    return "Sateinen";
    if (strcmp(c,"pouring")          == 0)    return "Kaatosade";
    if (strcmp(c,"snowy")            == 0)    return "Luminen";
    if (strcmp(c,"snowy-rainy")      == 0)    return "Rantasade";
    if (strcmp(c,"windy")            == 0)    return "Tuulinen";
    if (strcmp(c,"windy-variant")    == 0)    return "Tuulinen";
    if (strcmp(c,"fog")              == 0)    return "Sumuinen";
    if (strcmp(c,"hail")             == 0)    return "Raesade";
    if (strcmp(c,"lightning")        == 0)    return "Ukkonen";
    if (strcmp(c,"lightning-rainy")  == 0)    return "Ukkossade";
    return c;
}

/* ════════════════════════════════════════════════════════════════════
 * State structs
 * ═══════════════════════════════════════════════════════════════════ */

/* Status page: non-light tiles */
#define MAX_TILES  16
typedef struct {
    lv_obj_t  *tile;
    lv_obj_t  *icon_label;
    char       entity_id[80];
    uint32_t   accent;
} tile_state_t;

typedef struct {
    lv_obj_t  *indoor_label;
    lv_obj_t  *outdoor_label;
    char        indoor_entity[80];
    char        outdoor_entity[80];
} weather_state_t;

/* Lights page: one row per light.* entity */
#define MAX_LIGHTS 16
typedef struct {
    lv_obj_t  *toggle_btn;
    lv_obj_t  *icon_lbl;     /* label inside toggle button */
    lv_obj_t  *bri_label;    /* "75%" */
    lv_obj_t  *slider;
    char       entity_id[80];
    char       label[48];
    uint32_t   accent;
    int        brightness;   /* 0-100 */
    bool       is_on;
} light_row_t;

/* ════════════════════════════════════════════════════════════════════
 * Globals
 * ═══════════════════════════════════════════════════════════════════ */

static lv_obj_t       *s_page_status  = NULL;
static lv_obj_t       *s_page_lights  = NULL;
static int             s_active_page  = 0;
static lv_obj_t       *s_nav_btn[2]   = {NULL, NULL};
static lv_obj_t       *s_status_label = NULL;

static tile_state_t    s_tiles[MAX_TILES];
static int             s_tile_count   = 0;
static weather_state_t s_weather      = {0};

static light_row_t     s_lights[MAX_LIGHTS];
static int             s_light_count  = 0;

/* ════════════════════════════════════════════════════════════════════
 * Fade animation helpers
 * ═══════════════════════════════════════════════════════════════════ */

static void _opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void _fade_out_ready(lv_anim_t *a)
{
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

/* ════════════════════════════════════════════════════════════════════
 * Nav bar
 * ═══════════════════════════════════════════════════════════════════ */

static void nav_highlight(int active)
{
    for (int i = 0; i < 2; i++) {
        if (!s_nav_btn[i]) continue;
        bool on = (i == active);
        lv_obj_set_style_bg_color(s_nav_btn[i],
            lv_color_hex(on ? 0x1c2128 : C_BG), 0);
        lv_color_t col   = lv_color_hex(on ? C_GREEN : C_MUTED);
        lv_obj_t  *icon_c = lv_obj_get_child(s_nav_btn[i], 0);
        lv_obj_t  *text_c = lv_obj_get_child(s_nav_btn[i], 1);
        if (icon_c) lv_obj_set_style_text_color(icon_c, col, 0);
        if (text_c) lv_obj_set_style_text_color(text_c, col, 0);
    }
}

static void nav_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == s_active_page) return;

    lv_obj_t *from = (s_active_page == 0) ? s_page_status : s_page_lights;
    lv_obj_t *to   = (idx           == 0) ? s_page_status : s_page_lights;
    s_active_page  = idx;

    /* Bring new page to front, initially transparent */
    lv_obj_clear_flag(to, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(to, LV_OPA_TRANSP, 0);
    lv_obj_move_foreground(to);

    /* Fade in new page */
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, to);
    lv_anim_set_exec_cb(&a_in, _opa_anim_cb);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a_in, 220);
    lv_anim_start(&a_in);

    /* Fade out old page, then hide it */
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, from);
    lv_anim_set_exec_cb(&a_out, _opa_anim_cb);
    lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a_out, 180);
    lv_anim_set_completed_cb(&a_out, _fade_out_ready);
    lv_anim_start(&a_out);

    nav_highlight(idx);
}

static void build_nav_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCR_W, NAV_H);
    lv_obj_set_pos(bar, 0, CONTENT_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char *icons[]  = { LV_SYMBOL_HOME,  LV_SYMBOL_CHARGE };
    const char *labels[] = { "Koti",           "Valot"          };

    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_obj_create(bar);
        s_nav_btn[i] = btn;
        lv_obj_set_size(btn, SCR_W / 2, NAV_H);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_pad_row(btn, 4, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, nav_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *icon_lbl = lv_label_create(btn);
        lv_label_set_text(icon_lbl, icons[i]);
        lv_obj_set_style_text_font(icon_lbl, FONT_24, 0);
        lv_obj_set_style_text_color(icon_lbl,
            lv_color_hex(i == 0 ? C_GREEN : C_MUTED), 0);

        lv_obj_t *text_lbl = lv_label_create(btn);
        lv_label_set_text(text_lbl, labels[i]);
        lv_obj_set_style_text_font(text_lbl, FONT_16, 0);
        lv_obj_set_style_text_color(text_lbl,
            lv_color_hex(i == 0 ? C_GREEN : C_MUTED), 0);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Status page (page 0)
 * ═══════════════════════════════════════════════════════════════════ */

static void tile_event_cb(lv_event_t *e)
{
    tile_state_t *ts = (tile_state_t *)lv_event_get_user_data(e);
    if (ts->entity_id[0]) ha_toggle(ts->entity_id);
}

static void build_status_page(lv_obj_t *parent, cJSON *pages)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!pages || !cJSON_IsArray(pages)) return;
    cJSON *page_cfg = cJSON_GetArrayItem(pages, 0);
    if (!page_cfg) return;

    /* ── Weather card ── */
    cJSON *wcfg = cJSON_GetObjectItem(page_cfg, "weather");
    if (wcfg) {
        cJSON *ie = cJSON_GetObjectItem(wcfg, "indoor_entity");
        cJSON *oe = cJSON_GetObjectItem(wcfg, "outdoor_entity");
        if (ie && cJSON_IsString(ie)) {
            size_t n = strlen(ie->valuestring);
            if (n >= sizeof(s_weather.indoor_entity)) n = sizeof(s_weather.indoor_entity) - 1;
            memcpy(s_weather.indoor_entity, ie->valuestring, n);
            s_weather.indoor_entity[n] = '\0';
        }
        if (oe && cJSON_IsString(oe)) {
            size_t n = strlen(oe->valuestring);
            if (n >= sizeof(s_weather.outdoor_entity)) n = sizeof(s_weather.outdoor_entity) - 1;
            memcpy(s_weather.outdoor_entity, oe->valuestring, n);
            s_weather.outdoor_entity[n] = '\0';
        }

        lv_obj_t *wcard = lv_obj_create(parent);
        lv_obj_set_size(wcard, SCR_W - 32, 148);
        lv_obj_align(wcard, LV_ALIGN_TOP_MID, 0, 16);
        lv_obj_set_style_bg_color(wcard, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(wcard, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(wcard, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_border_width(wcard, 1, 0);
        lv_obj_set_style_radius(wcard, 16, 0);
        lv_obj_set_style_pad_all(wcard, 20, 0);
        lv_obj_clear_flag(wcard, LV_OBJ_FLAG_SCROLLABLE);

        s_weather.indoor_label = lv_label_create(wcard);
        lv_label_set_text(s_weather.indoor_label,
                          LV_SYMBOL_HOME "  Sisalla\n--.-°C");
        lv_obj_set_style_text_color(s_weather.indoor_label,
                                    lv_color_hex(C_BLUE), 0);
        lv_obj_set_style_text_font(s_weather.indoor_label,
                                   FONT_20, 0);
        lv_obj_align(s_weather.indoor_label, LV_ALIGN_LEFT_MID, 0, 0);

        s_weather.outdoor_label = lv_label_create(wcard);
        lv_label_set_text(s_weather.outdoor_label,
                          "Ulkona\n--.-°C\n---");
        lv_obj_set_style_text_color(s_weather.outdoor_label,
                                    lv_color_hex(C_MUTED), 0);
        lv_obj_set_style_text_font(s_weather.outdoor_label,
                                   FONT_20, 0);
        lv_obj_set_style_text_align(s_weather.outdoor_label,
                                    LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_weather.outdoor_label, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    /* ── Header row: title + WiFi status ── */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Smart Home");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, FONT_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 186);

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, LV_SYMBOL_WIFI "  Yhdistetaan...");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(C_YELLOW), 0);
    lv_obj_set_style_text_font(s_status_label, FONT_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -24, 192);

    /* Divider */
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, SCR_W - 32, 1);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_bg_color(div, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);

    /* ── Tile grid ── */
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_size(grid, SCR_W, CONTENT_H - 232);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 16, 0);
    lv_obj_set_style_pad_gap(grid, 16, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    /* Build tiles — skip light.* (those go to the lights page) */
    cJSON *tiles_arr = cJSON_GetObjectItem(page_cfg, "tiles");
    if (!tiles_arr || !cJSON_IsArray(tiles_arr)) return;

    cJSON *t;
    cJSON_ArrayForEach(t, tiles_arr) {
        if (s_tile_count >= MAX_TILES) break;

        cJSON *entity_j = cJSON_GetObjectItem(t, "entity");
        if (entity_j && cJSON_IsString(entity_j) &&
            strncmp(entity_j->valuestring, "light.", 6) == 0) continue;

        cJSON *label_j = cJSON_GetObjectItem(t, "label");
        cJSON *icon_j  = cJSON_GetObjectItem(t, "icon");
        cJSON *color_j = cJSON_GetObjectItem(t, "color");

        const char *label  = (label_j  && cJSON_IsString(label_j))
                             ? label_j->valuestring  : "?";
        const char *icon   = (icon_j   && cJSON_IsString(icon_j))
                             ? icon_j->valuestring   : "settings";
        uint32_t    accent = 0x888888;
        if (color_j && cJSON_IsString(color_j))
            accent = (uint32_t)strtoul(color_j->valuestring, NULL, 16);

        tile_state_t *ts = &s_tiles[s_tile_count++];
        ts->accent = accent;
        if (entity_j && cJSON_IsString(entity_j)) {
            size_t n = strlen(entity_j->valuestring);
            if (n >= sizeof(ts->entity_id)) n = sizeof(ts->entity_id) - 1;
            memcpy(ts->entity_id, entity_j->valuestring, n);
            ts->entity_id[n] = '\0';
        }

        lv_obj_t *tile = lv_obj_create(grid);
        ts->tile = tile;
        lv_obj_set_size(tile, 340, 300);
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(C_BORDER), 0);
        lv_obj_set_style_radius(tile, 20, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(accent), LV_STATE_PRESSED);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tile, tile_event_cb, LV_EVENT_CLICKED, ts);

        lv_obj_t *icon_lbl = lv_label_create(tile);
        ts->icon_label = icon_lbl;
        lv_label_set_text(icon_lbl, icon_sym(icon));
        lv_obj_set_style_text_font(icon_lbl, FONT_24, 0);
        lv_obj_set_style_text_color(icon_lbl, lv_color_hex(accent), 0);
        lv_obj_align(icon_lbl, LV_ALIGN_CENTER, 0, -24);

        lv_obj_t *name_lbl = lv_label_create(tile);
        lv_label_set_text(name_lbl, label);
        lv_obj_set_style_text_font(name_lbl, FONT_20, 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_align(name_lbl, LV_ALIGN_CENTER, 0, 24);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Lights page (page 1)
 * ═══════════════════════════════════════════════════════════════════ */

static void light_toggle_cb(lv_event_t *e)
{
    light_row_t *lr = (light_row_t *)lv_event_get_user_data(e);
    ha_toggle(lr->entity_id);
}

static void light_slider_cb(lv_event_t *e)
{
    light_row_t *lr = (light_row_t *)lv_event_get_user_data(e);
    int val = (int)lv_slider_get_value(lv_event_get_target(e));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(lr->bri_label, buf);
    ha_light_set(lr->entity_id, val, NULL);
}

static void build_lights_page(lv_obj_t *parent, cJSON *pages)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  Valot");
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(title, FONT_24, 0);
    lv_obj_set_pos(title, 24, 24);

    /* Scrollable list */
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, SCR_W, CONTENT_H - 68);
    lv_obj_set_pos(list, 0, 68);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_top(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 16, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    if (!pages || !cJSON_IsArray(pages)) return;

    cJSON *page;
    cJSON_ArrayForEach(page, pages) {
        cJSON *tiles_arr = cJSON_GetObjectItem(page, "tiles");
        if (!tiles_arr || !cJSON_IsArray(tiles_arr)) continue;
        cJSON *t;
        cJSON_ArrayForEach(t, tiles_arr) {
            if (s_light_count >= MAX_LIGHTS) break;
            cJSON *ej = cJSON_GetObjectItem(t, "entity");
            if (!ej || !cJSON_IsString(ej)) continue;
            if (strncmp(ej->valuestring, "light.", 6) != 0) continue;

            cJSON *lj = cJSON_GetObjectItem(t, "label");
            cJSON *cj = cJSON_GetObjectItem(t, "color");
            const char *lbl_str = (lj && cJSON_IsString(lj))
                                  ? lj->valuestring : ej->valuestring;
            uint32_t acc = 0xf0c040;
            if (cj && cJSON_IsString(cj))
                acc = (uint32_t)strtoul(cj->valuestring, NULL, 16);

            light_row_t *lr = &s_lights[s_light_count++];
            lr->accent     = acc;
            lr->brightness = 50;
            lr->is_on      = false;
            size_t n = strlen(ej->valuestring);
            if (n >= sizeof(lr->entity_id)) n = sizeof(lr->entity_id) - 1;
            memcpy(lr->entity_id, ej->valuestring, n);
            lr->entity_id[n] = '\0';
            n = strlen(lbl_str);
            if (n >= sizeof(lr->label)) n = sizeof(lr->label) - 1;
            memcpy(lr->label, lbl_str, n);
            lr->label[n] = '\0';

            /* ── Card ── */
            lv_obj_t *card = lv_obj_create(list);
            lv_obj_set_size(card, LV_PCT(100), 110);
            lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(card, lv_color_hex(C_BORDER), 0);
            lv_obj_set_style_border_width(card, 1, 0);
            lv_obj_set_style_radius(card, 16, 0);
            lv_obj_set_style_pad_hor(card, 16, 0);
            lv_obj_set_style_pad_ver(card, 14, 0);
            lv_obj_set_style_pad_column(card, 14, 0);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            /* Toggle button (circle) */
            lv_obj_t *tbtn = lv_button_create(card);
            lr->toggle_btn = tbtn;
            lv_obj_set_size(tbtn, 56, 56);
            lv_obj_set_style_radius(tbtn, 28, 0);
            lv_obj_set_style_bg_color(tbtn, lv_color_hex(C_DIM), 0);
            lv_obj_set_style_bg_opa(tbtn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(tbtn, 0, 0);
            lv_obj_set_style_shadow_width(tbtn, 0, 0);
            lv_obj_set_style_pad_all(tbtn, 0, 0);
            lv_obj_add_event_cb(tbtn, light_toggle_cb, LV_EVENT_CLICKED, lr);

            lv_obj_t *ticon = lv_label_create(tbtn);
            lr->icon_lbl = ticon;
            lv_label_set_text(ticon, LV_SYMBOL_CHARGE);
            lv_obj_set_style_text_font(ticon, FONT_20, 0);
            lv_obj_set_style_text_color(ticon, lv_color_hex(C_MUTED), 0);
            lv_obj_center(ticon);

            /* Name column (flex_grow) */
            lv_obj_t *name_col = lv_obj_create(card);
            lv_obj_set_flex_grow(name_col, 1);
            lv_obj_set_height(name_col, 56);
            lv_obj_set_style_bg_opa(name_col, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(name_col, 0, 0);
            lv_obj_set_style_pad_all(name_col, 0, 0);
            lv_obj_set_style_radius(name_col, 0, 0);
            lv_obj_clear_flag(name_col, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(name_col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(name_col, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

            lv_obj_t *name_lbl = lv_label_create(name_col);
            lv_label_set_text(name_lbl, lr->label);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name_lbl, LV_PCT(100));
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), 0);
            lv_obj_set_style_text_font(name_lbl, FONT_20, 0);

            /* Right column: bri% + slider (fixed 280 px) */
            lv_obj_t *rcol = lv_obj_create(card);
            lv_obj_set_size(rcol, 280, 56);
            lv_obj_set_style_bg_opa(rcol, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(rcol, 0, 0);
            lv_obj_set_style_pad_all(rcol, 0, 0);
            lv_obj_set_style_radius(rcol, 0, 0);
            lv_obj_clear_flag(rcol, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(rcol, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(rcol, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

            lv_obj_t *bri_lbl = lv_label_create(rcol);
            lr->bri_label = bri_lbl;
            lv_label_set_text(bri_lbl, "--");
            lv_obj_set_style_text_color(bri_lbl, lv_color_hex(C_MUTED), 0);
            lv_obj_set_style_text_font(bri_lbl, FONT_16, 0);

            lv_obj_t *slider = lv_slider_create(rcol);
            lr->slider = slider;
            lv_obj_set_size(slider, 280, 32);
            lv_slider_set_range(slider, 1, 100);
            lv_slider_set_value(slider, 50, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(slider, lv_color_hex(C_DIM), LV_PART_MAIN);
            lv_obj_set_style_bg_color(slider, lv_color_hex(acc), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(slider, lv_color_hex(C_TEXT), LV_PART_KNOB);
            lv_obj_set_style_radius(slider, 6, LV_PART_MAIN);
            lv_obj_set_style_radius(slider, 6, LV_PART_INDICATOR);
            lv_obj_set_style_radius(slider, 8, LV_PART_KNOB);
            lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
            lv_obj_add_event_cb(slider, light_slider_cb, LV_EVENT_RELEASED, lr);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════ */

void ui_manager_init(cJSON *config)
{
    s_tile_count   = 0;
    s_light_count  = 0;
    s_active_page  = 0;
    memset(&s_weather, 0, sizeof(s_weather));
    memset(s_tiles,    0, sizeof(s_tiles));
    memset(s_lights,   0, sizeof(s_lights));
    s_page_status  = NULL;
    s_page_lights  = NULL;
    s_nav_btn[0]   = NULL;
    s_nav_btn[1]   = NULL;
    s_status_label = NULL;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* Status page — fills content area, always visible initially */
    s_page_status = lv_obj_create(scr);
    lv_obj_set_size(s_page_status, SCR_W, CONTENT_H);
    lv_obj_set_pos(s_page_status, 0, 0);
    lv_obj_set_style_bg_color(s_page_status, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_page_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_page_status, 0, 0);
    lv_obj_set_style_border_width(s_page_status, 0, 0);
    lv_obj_set_style_radius(s_page_status, 0, 0);
    lv_obj_clear_flag(s_page_status, LV_OBJ_FLAG_SCROLLABLE);

    /* Lights page — same position, hidden initially */
    s_page_lights = lv_obj_create(scr);
    lv_obj_set_size(s_page_lights, SCR_W, CONTENT_H);
    lv_obj_set_pos(s_page_lights, 0, 0);
    lv_obj_set_style_bg_color(s_page_lights, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_page_lights, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_page_lights, 0, 0);
    lv_obj_set_style_border_width(s_page_lights, 0, 0);
    lv_obj_set_style_radius(s_page_lights, 0, 0);
    lv_obj_clear_flag(s_page_lights, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page_lights, LV_OBJ_FLAG_HIDDEN);

    cJSON *pages = cJSON_GetObjectItem(config, "pages");
    build_status_page(s_page_status, pages);
    build_lights_page(s_page_lights, pages);

    build_nav_bar(scr);
}

void ui_manager_on_state(const char *entity_id, const char *state,
                          cJSON *attributes)
{
    if (!bsp_display_lock(50)) return;

    /* Weather: indoor */
    if (s_weather.indoor_entity[0] &&
        strcmp(entity_id, s_weather.indoor_entity) == 0) {
        char buf[40];
        snprintf(buf, sizeof(buf),
                 LV_SYMBOL_HOME "  Sisalla\n%.1f°C", atof(state));
        lv_label_set_text(s_weather.indoor_label, buf);
        goto done;
    }

    /* Weather: outdoor */
    if (s_weather.outdoor_entity[0] &&
        strcmp(entity_id, s_weather.outdoor_entity) == 0) {
        float temp = 0;
        if (attributes) {
            cJSON *tj = cJSON_GetObjectItem(attributes, "temperature");
            if (tj && cJSON_IsNumber(tj)) temp = (float)tj->valuedouble;
        }
        char buf[48];
        snprintf(buf, sizeof(buf), "Ulkona\n%.1f°C\n%s",
                 temp, condition_str(state));
        lv_label_set_text(s_weather.outdoor_label, buf);
        goto done;
    }

    /* Status tiles */
    for (int i = 0; i < s_tile_count; i++) {
        tile_state_t *ts = &s_tiles[i];
        if (!ts->entity_id[0]) continue;
        if (strcmp(entity_id, ts->entity_id) != 0) continue;
        if (strncmp(entity_id, "scene.",      6) == 0 ||
            strncmp(entity_id, "script.",     7) == 0 ||
            strncmp(entity_id, "automation.", 11) == 0) break;
        bool on = (strcmp(state, "on")   == 0 ||
                   strcmp(state, "home") == 0 ||
                   strcmp(state, "open") == 0);
        lv_obj_set_style_bg_color(ts->tile,
            lv_color_hex(on ? ts->accent : C_DIM), 0);
        break;
    }

    /* Light rows */
    for (int i = 0; i < s_light_count; i++) {
        light_row_t *lr = &s_lights[i];
        if (strcmp(entity_id, lr->entity_id) != 0) continue;

        bool on = (strcmp(state, "on") == 0);
        lr->is_on = on;

        lv_obj_set_style_bg_color(lr->toggle_btn,
            lv_color_hex(on ? lr->accent : C_DIM), 0);
        lv_obj_set_style_text_color(lr->icon_lbl,
            lv_color_hex(on ? 0x111111 : C_MUTED), 0);

        if (on && attributes) {
            cJSON *bj = cJSON_GetObjectItem(attributes, "brightness");
            if (bj && cJSON_IsNumber(bj)) {
                lr->brightness = (int)(bj->valuedouble * 100.0 / 255.0 + 0.5);
                lv_slider_set_value(lr->slider, lr->brightness, LV_ANIM_OFF);
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", lr->brightness);
                lv_label_set_text(lr->bri_label, buf);
            }
        } else if (!on) {
            lv_label_set_text(lr->bri_label, "--");
        }
        break;
    }

done:
    bsp_display_unlock();
}

void ui_manager_set_status(const char *text, uint32_t colour)
{
    if (!s_status_label) return;
    if (bsp_display_lock(100)) {
        lv_label_set_text(s_status_label, text);
        lv_obj_set_style_text_color(s_status_label,
                                    lv_color_hex(colour), 0);
        bsp_display_unlock();
    }
}

void ui_manager_show_ap_screen(const char *ssid, const char *ip)
{
    if (!bsp_display_lock(200)) return;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(icon, FONT_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(C_YELLOW), 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -120);

    lv_obj_t *t1 = lv_label_create(scr);
    lv_label_set_text(t1, "WiFi Setup");
    lv_obj_set_style_text_font(t1, FONT_24, 0);
    lv_obj_set_style_text_color(t1, lv_color_hex(C_TEXT), 0);
    lv_obj_align(t1, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *t2 = lv_label_create(scr);
    char buf[80];
    snprintf(buf, sizeof(buf), "Yhdista WiFiin:\n%s", ssid);
    lv_label_set_text(t2, buf);
    lv_obj_set_style_text_font(t2, FONT_20, 0);
    lv_obj_set_style_text_color(t2, lv_color_hex(C_YELLOW), 0);
    lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *t3 = lv_label_create(scr);
    snprintf(buf, sizeof(buf), "Avaa selaimessa:\nhttp://%s", ip);
    lv_label_set_text(t3, buf);
    lv_obj_set_style_text_font(t3, FONT_16, 0);
    lv_obj_set_style_text_color(t3, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t3, LV_ALIGN_CENTER, 0, 120);

    bsp_display_unlock();
}
