#include "watch_menu.h"
#include "protocol.h"
#include <string.h>

LV_FONT_DECLARE(watch_menu_font);

enum {
    PANEL_WIDTH = 390,
    PANEL_HEIGHT = 450,
    CONTENT_X = 20,
    CONTENT_Y = 86,
    CONTENT_WIDTH = PANEL_WIDTH - CONTENT_X * 2,
    CONTENT_HEIGHT = PANEL_HEIGHT - CONTENT_Y - 20,
    MAX_WALLPAPERS = 5,
    INK = 0x10191f,
    SURFACE = 0x213139,
    PRESSED = 0x344a53,
    TEXT = 0xfff5e8,
    MUTED = 0xbccdd1,
    MINT = 0x9fe0d2,
    SAND = 0xeab884,
    SKY = 0xafcef2,
};

static struct {
    struct watch_menu_ops ops;
    enum watch_menu_page page;
    lv_obj_t *root;
    lv_obj_t *title;
    lv_obj_t *clock;
    lv_obj_t *body;
    lv_obj_t *footer;
    lv_obj_t *status;
    lv_obj_t *number;
    lv_obj_t *slider;
    lv_obj_t *percentage;
    lv_obj_t *tiles[MAX_WALLPAPERS];
    unsigned selected_wallpaper;
    unsigned stored_level;
    unsigned shown_level;
    char phone[WATCH_PHONE_SIZE];
} menu;

static const char *const page_titles[] = {
    [MENU_FUNCTIONS] = "功能菜单",
    [MENU_SETTINGS] = "设置",
    [MENU_CONTACTS] = "添加联系人",
    [MENU_VOLUME] = "音量",
    [MENU_BRIGHTNESS] = "亮度",
    [MENU_WALLPAPERS] = "切换壁纸",
    [MENU_WEATHER] = "天气",
};

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, value);
    return label;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height,
                        uint32_t color, lv_event_cb_t clicked, uintptr_t data)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(obj, lv_color_mix(lv_color_hex(PRESSED), lv_color_hex(color), 180), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 17, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_SHORT_CLICKED, (void *)data);
    return obj;
}

enum watch_menu_page watch_menu_parent(enum watch_menu_page page)
{
    switch (page) {
    case MENU_CONTACTS:
    case MENU_VOLUME:
    case MENU_BRIGHTNESS:
        return MENU_SETTINGS;
    case MENU_SETTINGS:
    case MENU_WALLPAPERS:
    case MENU_WEATHER:
        return MENU_FUNCTIONS;
    default:
        return MENU_NONE;
    }
}

static void navigate(lv_event_t *event)
{
    menu.ops.navigate((enum watch_menu_page)(uintptr_t)lv_event_get_user_data(event));
}

static void back(lv_event_t *event)
{
    (void)event;
    menu.ops.navigate(watch_menu_parent(menu.page));
}

static void navigation_row(int y, enum watch_menu_page target, const char *icon, const lv_font_t *icon_font,
                           uint32_t accent)
{
    lv_obj_t *row = button(menu.body, 0, y, CONTENT_WIDTH, 94, SURFACE, navigate, target);
    lv_obj_t *stamp = box(row, 15, 22, 50, 50, accent, 25);
    lv_obj_center(text(stamp, icon, icon_font, INK));
    lv_obj_align(text(row, page_titles[target], &watch_menu_font, TEXT), LV_ALIGN_LEFT_MID, 83, 0);
    lv_obj_align(text(row, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, MUTED), LV_ALIGN_RIGHT_MID, -22, 0);
}

static void phone_display(void)
{
    lv_label_set_text(menu.number, menu.phone[0] ? menu.phone : "输入号码");
    lv_obj_set_style_text_color(menu.number, lv_color_hex(menu.phone[0] ? TEXT : MUTED), 0);
}

static void confirm_contact(void)
{
    static const char *const messages[] = {
        [MENU_CONTACT_SAVED] = "已保存",
        [MENU_CONTACT_INVALID] = "号码无效",
        [MENU_CONTACT_DUPLICATE] = "号码已存在",
        [MENU_CONTACT_FULL] = "联系人已满",
        [MENU_CONTACT_SAVE_FAILED] = "保存失败",
    };
    char number[WATCH_PHONE_SIZE];
    const char *phone = menu.phone;
    if (strlen(phone) == 11 && phone[0] == '1') {
        memcpy(number, "+86", 3);
        memcpy(number + 3, phone, 12);
        phone = number;
    }
    const enum watch_menu_contact_result result = watch_valid_phone(phone) ?
                                                 menu.ops.add_contact(phone) : MENU_CONTACT_INVALID;
    lv_label_set_text(menu.status, messages[result]);
    if (result == MENU_CONTACT_SAVED) {
        menu.phone[0] = '\0';
        phone_display();
    }
}

static void phone_key(lv_event_t *event)
{
    const char key = (char)(uintptr_t)lv_event_get_user_data(event);
    const size_t length = strlen(menu.phone);
    if (key == '=') {
        confirm_contact();
        return;
    }
    if (key == '<') {
        if (length)
            menu.phone[length - 1] = '\0';
    } else if (key == '+') {
        if (menu.phone[0] == '+')
            memmove(menu.phone, menu.phone + 1, length);
        else if (length < sizeof(menu.phone) - 1) {
            memmove(menu.phone + 1, menu.phone, length + 1);
            menu.phone[0] = '+';
        }
    } else if (length < sizeof(menu.phone) - 1) {
        menu.phone[length] = key;
        menu.phone[length + 1] = '\0';
    }
    phone_display();
    lv_label_set_text(menu.status, "");
}

static int make_contacts(void)
{
    static const char keys[] = "123456789<0=";
    static const char *const key_labels[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", "确认",
    };
    menu.phone[0] = '\0';
    lv_obj_t *input = box(menu.body, 72, 0, CONTENT_WIDTH - 72, 58, SURFACE, 16);
    menu.number = text(input, "输入号码", &watch_menu_font, MUTED);
    lv_obj_set_width(menu.number, CONTENT_WIDTH - 90);
    lv_obj_set_style_text_align(menu.number, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(menu.number, LV_LABEL_LONG_CLIP);
    lv_obj_center(menu.number);
    lv_obj_t *plus = button(menu.body, 0, 0, 62, 58, SURFACE, phone_key, '+');
    lv_obj_center(text(plus, "+", &watch_menu_font, MINT));
    menu.status = text(menu.body, "", &watch_menu_font, SAND);
    lv_obj_set_pos(menu.status, 7, 64);
    for (unsigned i = 0; i < sizeof(keys) - 1; ++i) {
        const bool prepare = keys[i] == '=';
        lv_obj_t *key = button(menu.body, (int)(i % 3) * 120, 100 + (int)(i / 3) * 60,
                               110, 52, prepare ? MINT : SURFACE, phone_key, keys[i]);
        lv_obj_center(text(key, key_labels[i], keys[i] == '<' ? &lv_font_montserrat_48 : &watch_menu_font,
                           prepare ? INK : TEXT));
    }
    return 332;
}

static void show_percentage(unsigned level)
{
    lv_label_set_text_fmt(menu.percentage, "%u%%", level);
}

static void level_changed(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
        return;
    const unsigned level = (unsigned)lv_slider_get_value(menu.slider);
    if (code == LV_EVENT_VALUE_CHANGED && level != menu.shown_level) {
        if (menu.ops.set_level(menu.page, level, false)) {
            menu.shown_level = level;
            show_percentage(level);
            lv_label_set_text(menu.status, "");
        } else {
            lv_slider_set_value(menu.slider, (int32_t)menu.shown_level, LV_ANIM_OFF);
            lv_label_set_text(menu.status, "调整失败");
        }
    } else if (code == LV_EVENT_RELEASED && level != menu.stored_level) {
        const bool saved = menu.ops.set_level(menu.page, level, true);
        lv_label_set_text(menu.status, saved ? "" : "保存失败");
        if (saved) {
            menu.stored_level = level;
        } else if (menu.ops.set_level(menu.page, menu.stored_level, false)) {
            menu.shown_level = menu.stored_level;
            lv_slider_set_value(menu.slider, (int32_t)menu.shown_level, LV_ANIM_OFF);
            show_percentage(menu.shown_level);
        }
    }
}

static void sample_sound(lv_event_t *event)
{
    (void)event;
    lv_label_set_text(menu.status, menu.ops.play_sample() ? "" : "试听失败");
}

static int make_level(void)
{
    const bool audio = menu.page == MENU_VOLUME;
    const unsigned minimum = audio ? 0u : 10u;
    const unsigned saved = menu.ops.get_level(menu.page);
    menu.stored_level = LV_CLAMP(minimum, saved, 100u);
    menu.shown_level = menu.stored_level;
    menu.percentage = text(menu.body, "", &lv_font_montserrat_48, TEXT);
    lv_obj_align(menu.percentage, LV_ALIGN_TOP_MID, 0, 22);
    show_percentage(menu.shown_level);

    lv_obj_t *track = box(menu.body, 0, 109, CONTENT_WIDTH, 107, SURFACE, 19);
    menu.slider = lv_slider_create(track);
    lv_obj_remove_style_all(menu.slider);
    lv_obj_set_size(menu.slider, CONTENT_WIDTH - 72, 15);
    lv_obj_center(menu.slider);
    lv_obj_set_ext_click_area(menu.slider, 25);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(0x4e636a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(MINT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(menu.slider, 10, LV_PART_KNOB);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_slider_set_range(menu.slider, (int32_t)minimum, 100);
    lv_slider_set_value(menu.slider, (int32_t)menu.shown_level, LV_ANIM_OFF);
    lv_obj_add_event_cb(menu.slider, level_changed, LV_EVENT_ALL, NULL);
    if (audio) {
        lv_obj_t *sample = button(menu.body, 0, 236, CONTENT_WIDTH, 55, MINT, sample_sound, 0);
        lv_obj_center(text(sample, "试听", &watch_menu_font, INK));
    }
    menu.status = text(menu.body, "", &watch_menu_font, SAND);
    lv_obj_align(menu.status, LV_ALIGN_TOP_MID, 0, audio ? 301 : 235);
    return 332;
}

static void wallpaper_highlight(void)
{
    for (unsigned i = 0; i < MAX_WALLPAPERS; ++i) {
        if (!menu.tiles[i])
            continue;
        const bool selected = i == menu.selected_wallpaper;
        lv_obj_set_style_border_color(menu.tiles[i], lv_color_hex(selected ? MINT : SURFACE), 0);
    }
}

static void wallpaper_select(lv_event_t *event)
{
    menu.selected_wallpaper = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    wallpaper_highlight();
    lv_label_set_text(menu.status, "应用壁纸");
}

static void wallpaper_apply(lv_event_t *event)
{
    (void)event;
    const bool saved = menu.ops.apply_wallpaper(menu.selected_wallpaper);
    lv_label_set_text(menu.status, saved ? "已应用" : "保存失败");
}

static int make_wallpapers(void)
{
    const unsigned count = LV_MIN(menu.ops.wallpaper_count(), MAX_WALLPAPERS);
    if (!count) {
        lv_obj_center(text(menu.body, "暂无壁纸", &watch_menu_font, MUTED));
        return 0;
    }
    const unsigned persisted = menu.ops.get_wallpaper();
    menu.selected_wallpaper = persisted < count ? persisted : 0;
    const int tile_width = 168;
    const int tile_height = 128;
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *tile = button(menu.body, (int)(i % 2) * (tile_width + 14), (int)(i / 2) * (tile_height + 12),
                                tile_width, tile_height, SURFACE, wallpaper_select, i);
        menu.tiles[i] = tile;
        lv_obj_set_style_border_width(tile, 3, 0);
        lv_obj_set_style_border_post(tile, true, 0);
        const lv_img_dsc_t *thumbnail = menu.ops.wallpaper_thumbnail(i);
        if (thumbnail) {
            lv_obj_t *image = lv_image_create(tile);
            lv_image_set_src(image, thumbnail);
            lv_obj_set_size(image, 155, 83);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
            lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 5);
        }
        const char *name = menu.ops.wallpaper_name(i);
        lv_obj_t *label = text(tile, "", &watch_menu_font, TEXT);
        lv_obj_set_width(label, 148);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(label, "%u %s", i + 1, name && name[0] ? name : "壁纸");
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -1);
    }
    wallpaper_highlight();
    lv_obj_remove_flag(menu.footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(menu.body, 270);
    lv_obj_t *apply = button(menu.footer, 0, 0, CONTENT_WIDTH, 60, MINT, wallpaper_apply, 0);
    menu.status = text(apply, "应用壁纸", &watch_menu_font, INK);
    lv_obj_center(menu.status);
    return (int)((count + 1) / 2) * (tile_height + 12) - 12;
}

static int make_weather(void)
{
    static const char *const days[] = {"今天", "明天", "后天"};
    lv_obj_t *current = box(menu.body, 0, 0, CONTENT_WIDTH, 156, 0x263b52, 22);
    lv_obj_align(text(current, "--°", &lv_font_montserrat_48, TEXT), LV_ALIGN_TOP_MID, 0, 19);
    lv_obj_align(text(current, "暂无天气", &watch_menu_font, SKY), LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_t *forecast = box(menu.body, 0, 172, CONTENT_WIDTH, 128, SURFACE, 22);
    for (unsigned i = 0; i < sizeof(days) / sizeof(days[0]); ++i) {
        const int offset = ((int)i - 1) * 113;
        lv_obj_align(text(forecast, days[i], &watch_menu_font, TEXT), LV_ALIGN_TOP_MID, offset, 20);
        lv_obj_align(text(forecast, "-- / --", &watch_menu_font, MUTED), LV_ALIGN_TOP_MID, offset, 66);
    }
    return 300;
}

void watch_menu_init(lv_obj_t *parent, const struct watch_menu_ops *ops)
{
    menu.ops = *ops;
    menu.root = box(parent, 0, 0, PANEL_WIDTH, PANEL_HEIGHT, INK, 0);
    lv_obj_add_flag(menu.root, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *return_button = button(menu.root, 19, 17, 57, 56, SURFACE, back, 0);
    lv_obj_center(text(return_button, LV_SYMBOL_LEFT, &lv_font_montserrat_48, TEXT));
    menu.title = text(menu.root, "", &watch_menu_font, TEXT);
    lv_obj_align(menu.title, LV_ALIGN_TOP_LEFT, 88, 31);
    menu.clock = text(menu.root, "", &lv_font_montserrat_14, MUTED);
    lv_obj_align(menu.clock, LV_ALIGN_TOP_RIGHT, -24, 16);
    menu.body = box(menu.root, CONTENT_X, CONTENT_Y, CONTENT_WIDTH, CONTENT_HEIGHT, INK, 0);
    lv_obj_set_scroll_dir(menu.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu.body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(menu.body, lv_color_hex(0x91adb4), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(menu.body, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(menu.body, 3, LV_PART_SCROLLBAR);
    lv_obj_remove_flag(menu.body, LV_OBJ_FLAG_SCROLL_ELASTIC);
    menu.footer = box(menu.root, CONTENT_X, 367, CONTENT_WIDTH, 60, INK, 0);
    lv_obj_add_flag(menu.footer, LV_OBJ_FLAG_HIDDEN);
}

void watch_menu_show(enum watch_menu_page page)
{
    if (!menu.root || page == menu.page)
        return;
    if (menu.slider && menu.shown_level != menu.stored_level)
        menu.ops.set_level(menu.page, menu.stored_level, false);
    menu.page = page;
    menu.slider = NULL;
    memset(menu.tiles, 0, sizeof(menu.tiles));
    lv_obj_clean(menu.body);
    lv_obj_clean(menu.footer);
    lv_obj_add_flag(menu.footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(menu.body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(menu.body, CONTENT_HEIGHT);
    if (page == MENU_NONE) {
        lv_obj_add_flag(menu.root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(menu.title, page_titles[page]);
    int extent = 308;
    switch (page) {
    case MENU_FUNCTIONS:
        navigation_row(0, MENU_SETTINGS, LV_SYMBOL_SETTINGS, &lv_font_montserrat_48, SAND);
        navigation_row(107, MENU_WALLPAPERS, LV_SYMBOL_IMAGE, &lv_font_montserrat_48, MINT);
        navigation_row(214, MENU_WEATHER, "☁", &watch_menu_font, SKY);
        break;
    case MENU_SETTINGS:
        navigation_row(0, MENU_CONTACTS, LV_SYMBOL_PLUS, &lv_font_montserrat_48, SAND);
        navigation_row(107, MENU_VOLUME, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_48, MINT);
        navigation_row(214, MENU_BRIGHTNESS, "☀", &watch_menu_font, SKY);
        break;
    case MENU_CONTACTS:
        extent = make_contacts();
        break;
    case MENU_VOLUME:
    case MENU_BRIGHTNESS:
        extent = make_level();
        break;
    case MENU_WALLPAPERS:
        extent = make_wallpapers();
        break;
    case MENU_WEATHER:
        extent = make_weather();
        break;
    default:
        break;
    }
    lv_obj_remove_flag(menu.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(menu.body);
    lv_obj_scroll_to_y(menu.body, 0, LV_ANIM_OFF);
    if (extent > lv_obj_get_height(menu.body))
        lv_obj_add_flag(menu.body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

void watch_menu_set_time(const char *hhmm)
{
    if (menu.clock && strcmp(lv_label_get_text(menu.clock), hhmm))
        lv_label_set_text(menu.clock, hhmm);
}

bool watch_menu_blocks_swipe(const lv_obj_t *target)
{
    for (; target; target = lv_obj_get_parent(target)) {
        if (target == menu.slider)
            return true;
    }
    return false;
}
