#include "ui_lcd.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ui_lcd";

/* LVGL objects */
static lv_obj_t *lbl_status = NULL;
static lv_obj_t *lbl_asr = NULL;
static lv_obj_t *lbl_tts = NULL;
static lv_obj_t *lbl_hint = NULL;

/* Colors */
#define COLOR_BG         lv_color_hex(0x1a1a2e)
#define COLOR_STATUS_BG  lv_color_hex(0x16213e)
#define COLOR_ASR_TEXT   lv_color_hex(0x00ff88)
#define COLOR_TTS_TEXT   lv_color_hex(0x00aaff)
#define COLOR_HINT_TEXT  lv_color_hex(0x888888)
#define COLOR_WHITE      lv_color_hex(0xffffff)

int ui_lcd_init(void) {
    ESP_LOGI(TAG, "Initializing LCD UI...");

    /* Start BSP display + LVGL */
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return -1;
    }
    bsp_display_backlight_on();

    /* Build UI */
    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);

    /* ---- Top status bar (y=0, h=36) ---- */
    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, 320, 36);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, COLOR_STATUS_BG, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 4, 0);
    lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);

    lbl_status = lv_label_create(status_bar);
    lv_label_set_text(lbl_status, "Idle");
    lv_obj_set_style_text_color(lbl_status, COLOR_WHITE, 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_status);

    /* ---- Middle dialogue area (y=40, h=156) ---- */
    lv_obj_t *dialog_area = lv_obj_create(scr);
    lv_obj_set_size(dialog_area, 312, 156);
    lv_obj_set_pos(dialog_area, 4, 40);
    lv_obj_set_style_bg_opa(dialog_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dialog_area, 0, 0);
    lv_obj_set_style_pad_all(dialog_area, 4, 0);
    lv_obj_set_flex_flow(dialog_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dialog_area, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(dialog_area, LV_SCROLLBAR_MODE_AUTO);

    /* ASR label (user's speech) */
    lbl_asr = lv_label_create(dialog_area);
    lv_label_set_text(lbl_asr, "");
    lv_obj_set_width(lbl_asr, 300);
    lv_label_set_long_mode(lbl_asr, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_asr, COLOR_ASR_TEXT, 0);
    lv_obj_set_style_text_font(lbl_asr, &lv_font_montserrat_14, 0);

    /* TTS label (AI reply) */
    lbl_tts = lv_label_create(dialog_area);
    lv_label_set_text(lbl_tts, "");
    lv_obj_set_width(lbl_tts, 300);
    lv_label_set_long_mode(lbl_tts, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl_tts, COLOR_TTS_TEXT, 0);
    lv_obj_set_style_text_font(lbl_tts, &lv_font_montserrat_14, 0);

    /* ---- Bottom hint bar (y=200, h=40) ---- */
    lv_obj_t *hint_bar = lv_obj_create(scr);
    lv_obj_set_size(hint_bar, 320, 40);
    lv_obj_set_pos(hint_bar, 0, 200);
    lv_obj_set_style_bg_opa(hint_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hint_bar, 0, 0);
    lv_obj_set_style_pad_all(hint_bar, 4, 0);
    lv_obj_set_scrollbar_mode(hint_bar, LV_SCROLLBAR_MODE_OFF);

    lbl_hint = lv_label_create(hint_bar);
    lv_label_set_text(lbl_hint, "Say \"hi, jason\" or press button");
    lv_obj_set_style_text_color(lbl_hint, COLOR_HINT_TEXT, 0);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_hint);

    bsp_display_unlock();

    ESP_LOGI(TAG, "LCD UI initialized (320x240)");
    return 0;
}

void ui_lcd_set_state(const char *state_text) {
    if (!lbl_status) return;
    bsp_display_lock(0);
    lv_label_set_text(lbl_status, state_text);
    bsp_display_unlock();
}

void ui_lcd_set_asr_text(const char *text) {
    if (!lbl_asr) return;
    bsp_display_lock(0);
    lv_label_set_text(lbl_asr, text);
    /* Scroll dialog area to bottom */
    lv_obj_t *parent = lv_obj_get_parent(lbl_asr);
    if (parent) lv_obj_scroll_to_y(parent, LV_COORD_MAX, LV_ANIM_ON);
    bsp_display_unlock();
}

void ui_lcd_set_tts_text(const char *text) {
    if (!lbl_tts) return;
    bsp_display_lock(0);
    lv_label_set_text(lbl_tts, text);
    lv_obj_t *parent = lv_obj_get_parent(lbl_tts);
    if (parent) lv_obj_scroll_to_y(parent, LV_COORD_MAX, LV_ANIM_ON);
    bsp_display_unlock();
}

void ui_lcd_set_hint(const char *text) {
    if (!lbl_hint) return;
    bsp_display_lock(0);
    lv_label_set_text(lbl_hint, text);
    bsp_display_unlock();
}
