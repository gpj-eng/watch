#ifndef WATCH_MENU_H
#define WATCH_MENU_H

#include <lvgl.h>

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
    MENU_CONTACT_DUPLICATE,
    MENU_CONTACT_FULL,
    MENU_CONTACT_SAVE_FAILED,
};

struct watch_menu_ops {
    void (*navigate)(enum watch_menu_page page);
    enum watch_menu_contact_result (*add_contact)(const char *e164);
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
void watch_menu_set_time(const char *hhmm);
bool watch_menu_blocks_swipe(const lv_obj_t *target);

#endif
