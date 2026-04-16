#ifndef UI_LCD_H
#define UI_LCD_H

/**
 * Initialize LCD display with LVGL.
 * Creates the UI layout: status bar, dialogue area, hints.
 */
int ui_lcd_init(void);

/**
 * Update state display (e.g., "Idle", "Listening", "Speaking").
 */
void ui_lcd_set_state(const char *state_text);

/**
 * Update ASR recognized text (user's words).
 */
void ui_lcd_set_asr_text(const char *text);

/**
 * Update TTS reply text (AI response).
 */
void ui_lcd_set_tts_text(const char *text);

/**
 * Update bottom hint text.
 */
void ui_lcd_set_hint(const char *text);

#endif /* UI_LCD_H */
