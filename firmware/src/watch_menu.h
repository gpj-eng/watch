#ifndef WATCH_MENU_H
#define WATCH_MENU_H

#include <lvgl.h>
#include "protocol.h"

enum watch_menu_page {
    MENU_NONE,
    MENU_FUNCTIONS,
    MENU_SETTINGS,
    MENU_CONTACTS,
    MENU_VOLUME,
    MENU_BRIGHTNESS,
    MENU_WALLPAPERS,
    MENU_WEATHER,
};

enum watch_menu_contact_result {
    MENU_CONTACT_SAVED,
    MENU_CONTACT_INVALID,
    MENU_CONTACT_FULL,
    MENU_CONTACT_SAVE_FAILED,
};

struct watch_menu_ops {
    void (*navigate)(enum watch_menu_page page);
    enum watch_menu_contact_result (*save_contact)(const char *e164, bool emergency);
    bool (*get_contact)(unsigned index, char phone[WATCH_PHONE_SIZE], bool *emergency);
    unsigned (*get_level)(enum watch_menu_page page);
    bool (*set_level)(enum watch_menu_page page, unsigned percent, bool commit);
    bool (*play_sample)(void);
    unsigned (*wallpaper_count)(void);
    unsigned (*get_wallpaper)(void);
    const char *(*wallpaper_name)(unsigned index);
    const lv_img_dsc_t *(*wallpaper_thumbnail)(unsigned index);
    bool (*apply_wallpaper)(unsigned index);
};

void watch_menu_init(lv_obj_t *parent, const struct watch_menu_ops *ops);
void watch_menu_show(enum watch_menu_page page);
enum watch_menu_page watch_menu_parent(enum watch_menu_page page);
lv_obj_t *watch_menu_back_button(lv_obj_t *parent, lv_event_cb_t clicked);
void watch_menu_status(bool visible, const char *hhmm, bool connected, int battery_percent, bool charging);
void watch_menu_set_weather(const struct watch_weather *weather);
bool watch_menu_blocks_swipe(const lv_obj_t *target);

#endif
