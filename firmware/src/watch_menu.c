#include "watch_menu.h"
#include "protocol.h"
#include <string.h>

LV_FONT_DECLARE(watch_menu_font);
LV_FONT_DECLARE(watch_wallpaper_font);

enum {
    PANEL_WIDTH = 390,
    PANEL_HEIGHT = 450,
    CONTENT_X = 27,
    CONTENT_Y = 114,
    CONTENT_WIDTH = PANEL_WIDTH - CONTENT_X * 2,
    CONTENT_HEIGHT = PANEL_HEIGHT - CONTENT_Y - 30,
    MAX_WALLPAPERS = 5,
    INK = 0x080809,
    SURFACE = 0x262a35,
    PRESSED = 0x535b6c,
    TEXT = 0xf8faff,
    MUTED = 0xb8c4d7,
    MINT = 0x06aa49,
    SAND = 0xffd449,
    SKY = 0x43a6ff,
};

static struct {
    struct watch_menu_ops ops;
    enum watch_menu_page page;
    lv_obj_t *root;
    lv_obj_t *title;
    lv_obj_t *body;
    lv_obj_t *footer;
    lv_obj_t *status;
    lv_obj_t *number;
    lv_obj_t *emergency_box;
    lv_obj_t *contact_rows[WATCH_MAX_CONTACTS];
    lv_obj_t *contact_numbers[WATCH_MAX_CONTACTS];
    lv_obj_t *contact_tags[WATCH_MAX_CONTACTS];
    lv_obj_t *slider;
    lv_obj_t *percentage;
    lv_obj_t *tiles[MAX_WALLPAPERS];
    unsigned selected_wallpaper;
    unsigned stored_level;
    unsigned shown_level;
    bool emergency;
    char phone[WATCH_PHONE_SIZE];
} menu;

static struct {
    lv_obj_t *root, *clock, *bluetooth, *battery_fill, *battery_unknown;
    int battery;
    bool connected, charging;
} status_bar;

static struct watch_weather forecast;
static bool weather_available;

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

static void gradient(lv_obj_t *obj, uint32_t top, uint32_t bottom, int radius)
{
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bottom), LV_STATE_PRESSED);
}

static lv_obj_t *outline(lv_obj_t *parent, int x, int y, int w, int h, int radius, int width, uint32_t color)
{
    lv_obj_t *obj = box(parent, x, y, w, h, INK, radius);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *stroke(lv_obj_t *parent, const lv_point_precise_t *points, unsigned count,
                        int width, uint32_t color)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_line_set_points(line, points, count);
    return line;
}

#define STROKE(parent, points, width, color) \
    stroke(parent, points, sizeof(points) / sizeof((points)[0]), width, color)

static void chevron(lv_obj_t *parent, int x, int y, bool left)
{
    static const lv_point_precise_t previous[] = {{15, 3}, {4, 14}, {15, 25}};
    static const lv_point_precise_t next[] = {{4, 3}, {15, 14}, {4, 25}};
    lv_obj_t *mark = box(parent, x, y, 20, 29, INK, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    stroke(mark, left ? previous : next, 3, 4, TEXT);
}

lv_obj_t *watch_menu_back_button(lv_obj_t *parent, lv_event_cb_t clicked)
{
    lv_obj_t *back_button = button(parent, 24, 46, 53, 52, INK, clicked, 0);
    lv_obj_set_style_bg_opa(back_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(back_button, LV_OPA_40, LV_STATE_PRESSED);
    chevron(back_button, 11, 12, true);
    return back_button;
}

static void sun(lv_obj_t *parent)
{
    static const lv_point_precise_t rays[][2] = {
        {{24, 3}, {24, 8}}, {{39, 9}, {35, 13}}, {{45, 24}, {40, 24}}, {{39, 39}, {35, 35}},
        {{24, 45}, {24, 40}}, {{9, 39}, {13, 35}}, {{3, 24}, {8, 24}}, {{9, 9}, {13, 13}},
    };
    outline(parent, 12, 12, 25, 25, LV_RADIUS_CIRCLE, 4, TEXT);
    for (unsigned i = 0; i < sizeof(rays) / sizeof(rays[0]); ++i)
        STROKE(parent, rays[i], 4, TEXT);
}

static void cloud_body(lv_obj_t *parent)
{
    box(parent, 3, 28, 35, 15, TEXT, LV_RADIUS_CIRCLE);
    box(parent, 4, 23, 19, 19, TEXT, LV_RADIUS_CIRCLE);
    box(parent, 15, 19, 20, 20, TEXT, LV_RADIUS_CIRCLE);
}

static void clouds(lv_obj_t *parent)
{
    static const lv_point_precise_t rays[][2] = {
        {{27, 2}, {27, 7}}, {{37, 6}, {34, 10}}, {{44, 16}, {40, 18}}, {{44, 27}, {40, 26}},
        {{17, 4}, {20, 8}},
    };
    for (unsigned i = 0; i < sizeof(rays) / sizeof(rays[0]); ++i)
        STROKE(parent, rays[i], 3, SAND);
    box(parent, 17, 10, 23, 23, SAND, LV_RADIUS_CIRCLE);
    cloud_body(parent);
}

LV_IMAGE_DECLARE(watch_icon_settings);
LV_IMAGE_DECLARE(watch_icon_wallpaper);
LV_IMAGE_DECLARE(watch_icon_weather);
LV_IMAGE_DECLARE(watch_icon_contacts);
LV_IMAGE_DECLARE(watch_icon_volume);
LV_IMAGE_DECLARE(watch_icon_brightness);

static void row_icon(lv_obj_t *parent, enum watch_menu_page target)
{
    static const lv_image_dsc_t *const icons[] = {
        [MENU_SETTINGS] = &watch_icon_settings,
        [MENU_WALLPAPERS] = &watch_icon_wallpaper,
        [MENU_WEATHER] = &watch_icon_weather,
        [MENU_CONTACTS] = &watch_icon_contacts,
        [MENU_VOLUME] = &watch_icon_volume,
        [MENU_BRIGHTNESS] = &watch_icon_brightness,
    };
    lv_obj_t *icon = lv_image_create(parent);
    lv_image_set_src(icon, icons[target]);
    lv_obj_set_pos(icon, 31, 15);
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

static void navigation_row(int y, enum watch_menu_page target, uint32_t top, uint32_t bottom)
{
    lv_obj_t *row = button(menu.body, 0, y, CONTENT_WIDTH, 78, bottom, navigate, target);
    gradient(row, top, bottom, LV_RADIUS_CIRCLE);
    row_icon(row, target);
    lv_obj_align(text(row, page_titles[target], &watch_menu_font, TEXT), LV_ALIGN_LEFT_MID, 102, 0);
    chevron(row, CONTENT_WIDTH - 40, 25, false);
}

static void phone_display(void)
{
    lv_obj_set_style_text_font(menu.number, strlen(menu.phone) > 12 ? &lv_font_montserrat_22 : &watch_menu_font, 0);
    lv_label_set_text(menu.number, menu.phone);
}

static void update_emergency_check(void)
{
    lv_obj_set_style_bg_opa(menu.emergency_box, menu.emergency ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void toggle_emergency(lv_event_t *event)
{
    (void)event;
    menu.emergency = !menu.emergency;
    update_emergency_check();
}

static bool refresh_contacts(void)
{
    bool has_saved = false;
    for (unsigned i = 0; i < WATCH_MAX_CONTACTS; ++i) {
        char phone[WATCH_PHONE_SIZE];
        bool emergency;
        if (menu.ops.get_contact(i, phone, &emergency)) {
            lv_obj_remove_flag(menu.contact_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(menu.contact_numbers[i], phone);
            lv_label_set_text(menu.contact_tags[i], emergency ? "紧急" : "");
            has_saved = true;
        } else {
            lv_obj_add_flag(menu.contact_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (has_saved)
        lv_obj_add_flag(menu.body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return has_saved;
}

static void edit_contact(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (!menu.ops.get_contact(index, menu.phone, &menu.emergency))
        return;
    phone_display();
    update_emergency_check();
    lv_label_set_text(menu.status, "");
    lv_obj_scroll_to_y(menu.body, 0, LV_ANIM_OFF);
}

static void confirm_contact(void)
{
    static const char *const messages[] = {
        [MENU_CONTACT_SAVED] = "已保存",
        [MENU_CONTACT_INVALID] = "号码无效",
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
                                                 menu.ops.save_contact(phone, menu.emergency) : MENU_CONTACT_INVALID;
    lv_label_set_text(menu.status, messages[result]);
    if (result == MENU_CONTACT_SAVED) {
        menu.phone[0] = '\0';
        menu.emergency = false;
        phone_display();
        update_emergency_check();
        refresh_contacts();
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
    menu.emergency = false;
    lv_obj_set_y(menu.body, 101);
    lv_obj_set_height(menu.body, 342);
    lv_obj_t *input_title = text(menu.body, "输入号码", &watch_menu_font, TEXT);
    lv_obj_set_pos(input_title, 4, 0);
    lv_obj_t *input = box(menu.body, 49, 32, CONTENT_WIDTH - 49, 48, 0x202735, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x40506b), 0);
    menu.number = text(input, "", &watch_menu_font, TEXT);
    lv_obj_set_width(menu.number, CONTENT_WIDTH - 73);
    lv_obj_set_style_text_align(menu.number, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(menu.number, LV_LABEL_LONG_CLIP);
    lv_obj_center(menu.number);
    lv_obj_t *plus = button(menu.body, 0, 32, 43, 48, SURFACE, phone_key, '+');
    lv_obj_center(text(plus, "+", &watch_menu_font, TEXT));
    lv_obj_t *emergency = button(menu.body, 0, 83, CONTENT_WIDTH, 41, INK, toggle_emergency, 0);
    lv_obj_set_style_bg_opa(emergency, LV_OPA_TRANSP, 0);
    menu.emergency_box = outline(emergency, 6, 9, 23, 23, 5, 2, MINT);
    lv_obj_set_style_bg_color(menu.emergency_box, lv_color_hex(MINT), 0);
    lv_obj_align(text(emergency, "紧急联系人", &watch_menu_font, TEXT), LV_ALIGN_LEFT_MID, 39, 0);
    update_emergency_check();
    menu.status = text(menu.body, "", &watch_menu_font, TEXT);
    lv_obj_set_pos(menu.status, 5, 311);
    for (unsigned i = 0; i < sizeof(keys) - 1; ++i) {
        const bool confirm = keys[i] == '=';
        lv_obj_t *key = button(menu.body, (int)(i % 3) * 114, 128 + (int)(i / 3) * 46,
                               108, 42, confirm ? MINT : SURFACE, phone_key, keys[i]);
        gradient(key, confirm ? 0x10d659 : 0x303541, confirm ? 0x05813e : 0x262a35, 12);
        lv_obj_center(text(key, key_labels[i], keys[i] == '<' ? &lv_font_montserrat_22 : &watch_menu_font, TEXT));
    }
    for (unsigned i = 0; i < WATCH_MAX_CONTACTS; ++i) {
        menu.contact_rows[i] = button(menu.body, 0, 350 + (int)i * 58, CONTENT_WIDTH, 51,
                                      SURFACE, edit_contact, i);
        menu.contact_numbers[i] = text(menu.contact_rows[i], "", &lv_font_montserrat_22, TEXT);
        lv_obj_align(menu.contact_numbers[i], LV_ALIGN_LEFT_MID, 13, 0);
        menu.contact_tags[i] = text(menu.contact_rows[i], "", &watch_menu_font, SKY);
        lv_obj_align(menu.contact_tags[i], LV_ALIGN_RIGHT_MID, -11, 0);
    }
    return refresh_contacts() ? 351 + WATCH_MAX_CONTACTS * 58 : 342;
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
    lv_obj_align(menu.percentage, LV_ALIGN_TOP_MID, 0, 14);
    show_percentage(menu.shown_level);

    lv_obj_t *track = box(menu.body, 0, 100, CONTENT_WIDTH, 96, SURFACE, 24);
    menu.slider = lv_slider_create(track);
    lv_obj_remove_style_all(menu.slider);
    lv_obj_set_size(menu.slider, CONTENT_WIDTH - 72, 15);
    lv_obj_center(menu.slider);
    lv_obj_set_ext_click_area(menu.slider, 25);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(0x4e636a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(audio ? 0x9563f1 : 0x1ec4b0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(menu.slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(menu.slider, 10, LV_PART_KNOB);
    lv_obj_set_style_bg_color(menu.slider, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(menu.slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_slider_set_range(menu.slider, (int32_t)minimum, 100);
    lv_slider_set_value(menu.slider, (int32_t)menu.shown_level, LV_ANIM_OFF);
    lv_obj_add_event_cb(menu.slider, level_changed, LV_EVENT_ALL, NULL);
    if (audio) {
        lv_obj_t *sample = button(menu.body, 0, 216, CONTENT_WIDTH, 55, 0x403074, sample_sound, 0);
        gradient(sample, 0x7951b4, 0x3c2867, LV_RADIUS_CIRCLE);
        lv_obj_center(text(sample, "试听", &watch_menu_font, TEXT));
    }
    menu.status = text(menu.body, "", &watch_menu_font, SAND);
    lv_obj_align(menu.status, LV_ALIGN_TOP_MID, 0, audio ? 273 : 213);
    return 305;
}

static void wallpaper_highlight(void)
{
    for (unsigned i = 0; i < MAX_WALLPAPERS; ++i) {
        if (!menu.tiles[i])
            continue;
        const bool selected = i == menu.selected_wallpaper;
        lv_obj_set_style_border_color(menu.tiles[i], lv_color_hex(selected ? SKY : SURFACE), 0);
        lv_obj_t *mark = lv_obj_get_child(menu.tiles[i], -1);
        if (selected)
            lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
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
    const int tile_width = 162;
    const int tile_height = 115;
    for (unsigned i = 0; i < count; ++i) {
        lv_obj_t *tile = button(menu.body, (int)(i % 2) * (tile_width + 12), (int)(i / 2) * (tile_height + 11),
                                tile_width, tile_height, SURFACE, wallpaper_select, i);
        menu.tiles[i] = tile;
        lv_obj_set_style_radius(tile, 21, 0);
        lv_obj_set_style_clip_corner(tile, true, 0);
        lv_obj_set_style_border_width(tile, 3, 0);
        lv_obj_set_style_border_post(tile, true, 0);
        const lv_img_dsc_t *thumbnail = menu.ops.wallpaper_thumbnail(i);
        if (thumbnail) {
            lv_obj_t *image = lv_image_create(tile);
            lv_image_set_src(image, thumbnail);
            lv_obj_set_size(image, tile_width, tile_height);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
            lv_obj_center(image);
        }
        const char *name = menu.ops.wallpaper_name(i);
        lv_obj_t *shade = box(tile, 0, tile_height - 31, tile_width, 31, INK, 0);
        lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
        lv_obj_t *label = text(shade, "", &watch_wallpaper_font, TEXT);
        lv_obj_set_width(label, tile_width - 45);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text_fmt(label, "%u %s", i + 1, name && name[0] ? name : "壁纸");
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *mark = box(tile, tile_width - 34, tile_height - 35, 27, 27, 0x2876df, LV_RADIUS_CIRCLE);
        lv_obj_set_style_border_width(mark, 1, 0);
        lv_obj_set_style_border_color(mark, lv_color_hex(TEXT), 0);
        lv_obj_center(text(mark, LV_SYMBOL_OK, &lv_font_montserrat_14, TEXT));
    }
    wallpaper_highlight();
    lv_obj_remove_flag(menu.footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(menu.body, 241);
    lv_obj_t *apply = button(menu.footer, 13, 0, CONTENT_WIDTH - 26, 57, 0x124caa, wallpaper_apply, 0);
    gradient(apply, 0x367fee, 0x124caa, LV_RADIUS_CIRCLE);
    menu.status = text(apply, "应用壁纸", &watch_menu_font, TEXT);
    lv_obj_center(menu.status);
    return (int)((count + 1) / 2) * (tile_height + 11) - 11;
}

static const char *weather_description(uint8_t code)
{
    if (code == 0)
        return "晴";
    if (code <= 2)
        return "多云";
    if (code == 3)
        return "阴";
    if (code == 45 || code == 48)
        return "雾";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return "雪";
    if (code >= 95 && code <= 99)
        return "雷雨";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
        return "雨";
    return "暂无天气";
}

static void weather_glyph(lv_obj_t *parent, uint8_t condition)
{
    if (condition == 0) {
        sun(parent);
    } else if (condition <= 2 || condition == WATCH_WEATHER_MISSING_U8) {
        clouds(parent);
    } else {
        cloud_body(parent);
        if (condition >= 51) {
            static const lv_point_precise_t drops[][2] = {
                {{17, 42}, {15, 48}}, {{26, 42}, {24, 48}}, {{35, 42}, {33, 48}},
            };
            for (unsigned i = 0; i < sizeof(drops) / sizeof(drops[0]); ++i)
                STROKE(parent, drops[i], 2, SKY);
        }
    }
}

static void format_temperature(char *buffer, size_t length, int temperature)
{
    if (temperature == WATCH_WEATHER_MISSING_S8)
        lv_snprintf(buffer, length, "--");
    else
        lv_snprintf(buffer, length, "%d°", temperature);
}

static int make_weather(void)
{
    static const char *const days[] = {"今天", "明天", "后天"};
    lv_obj_t *current = box(menu.body, 0, 0, CONTENT_WIDTH, 158, 0x164e8a, 22);
    gradient(current, 0x1767aa, 0x255887, 22);
    lv_obj_t *weather_icon = box(current, 48, 22, 48, 48, INK, 0);
    lv_obj_set_style_bg_opa(weather_icon, LV_OPA_TRANSP, 0);
    weather_glyph(weather_icon, weather_available ? forecast.current_wmo : WATCH_WEATHER_MISSING_U8);
    char temperature[16] = "--";
    if (weather_available && forecast.temperature_deci_c != WATCH_WEATHER_MISSING_S16) {
        int t = forecast.temperature_deci_c;
        lv_snprintf(temperature, sizeof(temperature), "%d°C", (t + (t < 0 ? -5 : 5)) / 10);
    }
    lv_obj_align(text(current, temperature, &lv_font_montserrat_48, TEXT), LV_ALIGN_TOP_MID, 54, 4);
    lv_obj_align(text(current, weather_available ? weather_description(forecast.current_wmo) : "暂无天气",
                      &watch_menu_font, TEXT), LV_ALIGN_TOP_MID, 54, 59);
    box(current, 16, 97, CONTENT_WIDTH - 32, 1, 0x72a5d1, 0);
    lv_obj_set_pos(text(current, "湿度", &watch_menu_font, TEXT), 13, 110);
    lv_obj_set_pos(text(current, "风速", &watch_menu_font, TEXT), 172, 110);
    char humidity[8] = "--", wind[16] = "--";
    if (weather_available && forecast.humidity_percent != WATCH_WEATHER_MISSING_U8)
        lv_snprintf(humidity, sizeof(humidity), "%u%%", (unsigned)forecast.humidity_percent);
    if (weather_available && forecast.wind_kmh != WATCH_WEATHER_MISSING_U8)
        lv_snprintf(wind, sizeof(wind), "%u km/h", (unsigned)forecast.wind_kmh);
    lv_obj_set_pos(text(current, humidity, &lv_font_montserrat_22, TEXT), 83, 116);
    lv_obj_set_pos(text(current, wind, &lv_font_montserrat_18, TEXT), 241, 118);
    lv_obj_t *forecast_card = box(menu.body, 0, 169, CONTENT_WIDTH, 113, SURFACE, 22);
    for (unsigned i = 0; i < sizeof(days) / sizeof(days[0]); ++i) {
        const int offset = ((int)i - 1) * 108;
        if (i)
            box(forecast_card, 115 + (int)(i - 1) * 108, 12, 1, 89, 0x424b5a, 0);
        lv_obj_align(text(forecast_card, days[i], &watch_menu_font, TEXT), LV_ALIGN_TOP_MID, offset, 4);
        uint8_t code = weather_available ? forecast.daily_wmo[i] : WATCH_WEATHER_MISSING_U8;
        if (weather_available && code != WATCH_WEATHER_MISSING_U8) {
            lv_obj_t *icon = box(forecast_card, 0, 0, 48, 48, INK, 0);
            lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, offset, 31);
            weather_glyph(icon, code);
        }
        char high[8], low[8], range[20];
        format_temperature(high, sizeof(high), weather_available ? forecast.daily_high_c[i] : WATCH_WEATHER_MISSING_S8);
        format_temperature(low, sizeof(low), weather_available ? forecast.daily_low_c[i] : WATCH_WEATHER_MISSING_S8);
        lv_snprintf(range, sizeof(range), "%s / %s", high, low);
        lv_obj_align(text(forecast_card, range, &lv_font_montserrat_22, TEXT), LV_ALIGN_TOP_MID, offset, 78);
    }
    return 282;
}

void watch_menu_set_weather(const struct watch_weather *weather)
{
    if (weather_available == (weather != NULL) && (!weather || forecast.fetched_at_utc == weather->fetched_at_utc))
        return;
    weather_available = weather != NULL;
    if (weather)
        forecast = *weather;
    if (menu.page == MENU_WEATHER) {
        menu.page = MENU_NONE;
        watch_menu_show(MENU_WEATHER);
    }
}

void watch_menu_init(lv_obj_t *parent, const struct watch_menu_ops *ops)
{
    menu.ops = *ops;
    menu.root = box(parent, 0, 0, PANEL_WIDTH, PANEL_HEIGHT, INK, 0);
    lv_obj_add_flag(menu.root, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    watch_menu_back_button(menu.root, back);
    menu.title = text(menu.root, "", &watch_menu_font, TEXT);
    lv_obj_align(menu.title, LV_ALIGN_TOP_MID, 0, 53);
    menu.body = box(menu.root, CONTENT_X, CONTENT_Y, CONTENT_WIDTH, CONTENT_HEIGHT, INK, 0);
    lv_obj_set_scroll_dir(menu.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu.body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(menu.body, lv_color_hex(0x91adb4), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(menu.body, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(menu.body, 3, LV_PART_SCROLLBAR);
    lv_obj_remove_flag(menu.body, LV_OBJ_FLAG_SCROLL_ELASTIC);
    menu.footer = box(menu.root, CONTENT_X, 367, CONTENT_WIDTH, 57, INK, 0);
    lv_obj_add_flag(menu.footer, LV_OBJ_FLAG_HIDDEN);

    static const lv_point_precise_t bluetooth[] = {
        {4, 7}, {18, 20}, {11, 27}, {11, 2}, {18, 8}, {4, 22},
    };
    status_bar.root = box(parent, 0, 0, PANEL_WIDTH, 43, INK, 0);
    lv_obj_set_style_bg_opa(status_bar.root, LV_OPA_TRANSP, 0);
    status_bar.clock = text(status_bar.root, "--:--", &lv_font_montserrat_22, TEXT);
    lv_obj_set_pos(status_bar.clock, 44, 16);
    status_bar.bluetooth = STROKE(status_bar.root, bluetooth, 2, MUTED);
    lv_obj_set_pos(status_bar.bluetooth, 274, 12);
    outline(status_bar.root, 308, 16, 36, 18, 4, 2, TEXT);
    box(status_bar.root, 344, 21, 3, 8, TEXT, 1);
    status_bar.battery_fill = box(status_bar.root, 312, 20, 29, 10, 0x28db35, 2);
    status_bar.battery_unknown = box(status_bar.root, 321, 24, 10, 2, MUTED, 1);
    status_bar.battery = -2;
    lv_obj_add_flag(status_bar.root, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_y(menu.body, CONTENT_Y);
    lv_obj_set_height(menu.body, CONTENT_HEIGHT);
    if (page == MENU_NONE) {
        lv_obj_add_flag(menu.root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(menu.title, page_titles[page]);
    int extent = 254;
    switch (page) {
    case MENU_FUNCTIONS:
        navigation_row(0, MENU_SETTINGS, 0x244b87, 0x182f52);
        navigation_row(88, MENU_WALLPAPERS, 0x63479b, 0x402660);
        navigation_row(176, MENU_WEATHER, 0x128b85, 0x07575d);
        break;
    case MENU_SETTINGS:
        navigation_row(0, MENU_CONTACTS, 0x1c73f4, 0x0748ad);
        navigation_row(88, MENU_VOLUME, 0x684b9e, 0x422874);
        navigation_row(176, MENU_BRIGHTNESS, 0x129a8e, 0x075e64);
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

void watch_menu_status(bool visible, const char *hhmm, bool connected, int battery_percent, bool charging)
{
    if (!status_bar.root)
        return;
    if (!visible) {
        lv_obj_add_flag(status_bar.root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(status_bar.root, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(lv_label_get_text(status_bar.clock), hhmm))
        lv_label_set_text(status_bar.clock, hhmm);
    if (status_bar.connected != connected) {
        status_bar.connected = connected;
        lv_obj_set_style_line_color(status_bar.bluetooth, lv_color_hex(connected ? SKY : MUTED), 0);
    }
    if (status_bar.battery != battery_percent || status_bar.charging != charging) {
        status_bar.battery = battery_percent;
        status_bar.charging = charging;
        if (battery_percent < 0) {
            lv_obj_add_flag(status_bar.battery_fill, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(status_bar.battery_unknown, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(status_bar.battery_unknown, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(status_bar.battery_fill, battery_percent * 28 / 100);
            lv_obj_set_style_bg_color(status_bar.battery_fill,
                                      lv_color_hex(charging ? 0x28db35 : 0xffffff), 0);
            lv_obj_remove_flag(status_bar.battery_fill, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

bool watch_menu_blocks_swipe(const lv_obj_t *target)
{
    for (; target; target = lv_obj_get_parent(target)) {
        if (target == menu.slider)
            return true;
    }
    return false;
}
