#include "app_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "app_logger.h"
#include "app_sd.h"
#include "font_cn_16.h"

#define FILE_LIST_MAX 80
#define FILE_PAGE 4
#define NAME_LEN 40
#define VIEW_CAP 768

static const char *TAG = "files";

static char s_dir[96] = APP_SD_MOUNT;
static char s_names[FILE_LIST_MAX][NAME_LEN];
static uint8_t s_is_dir[FILE_LIST_MAX];
static uint32_t s_sizes[FILE_LIST_MAX];
static int s_count;
static int s_list_page;
static bool s_truncated;
static bool s_ready;
static bool s_viewing;
static bool s_follow_tail;
static char s_view_path[140];
static long s_view_size;
static long s_view_offset;
static long s_view_end;
static char s_chunk[VIEW_CAP + 1];

static lv_obj_t *s_list_box;
static lv_obj_t *s_view_box;
static lv_obj_t *s_items[FILE_PAGE];
static lv_obj_t *s_info;
static lv_obj_t *s_text;
static lv_obj_t *s_title;
static lv_obj_t *s_scroll;
static lv_obj_t *s_mid_label;

static void show_list(void);
static void show_view(void);

static void use_cn_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &font_cn_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, &font_cn_16, LV_PART_ITEMS);
}

static void set_parent_dir(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        snprintf(path, 96, "%s", APP_SD_MOUNT);
        return;
    }
    *slash = '\0';
    if (path[0] == '\0') {
        snprintf(path, 96, "%s", APP_SD_MOUNT);
    }
}

static const char *dir_title(void)
{
    static char title[96];
    size_t prefix = strlen(APP_SD_MOUNT);
    if (strncmp(s_dir, APP_SD_MOUNT, prefix) == 0 && (s_dir[prefix] == '\0' || s_dir[prefix] == '/')) {
        if (s_dir[prefix] == '\0' || s_dir[prefix + 1] == '\0') {
            return "SD";
        }
        const char *rest = s_dir + prefix + 1;
        size_t len = strlen(rest);
        if (len >= sizeof(title)) {
            len = sizeof(title) - 1;
        }
        memcpy(title, rest, len);
        title[len] = '\0';
        return title;
    }
    return s_dir;
}

static bool join_path(char *dst, size_t dst_len, const char *dir, const char *name)
{
    int wrote = snprintf(dst, dst_len, "%s/%s", dir, name);
    return wrote > 0 && (size_t)wrote < dst_len;
}

static void format_size(char *out, size_t out_len, uint32_t bytes)
{
    if (bytes < 1024) {
        snprintf(out, out_len, "%luB", (unsigned long)bytes);
    } else if (bytes < 1024UL * 1024UL) {
        snprintf(out, out_len, "%luK", (unsigned long)((bytes + 512UL) / 1024UL));
    } else {
        snprintf(out, out_len, "%luM", (unsigned long)((bytes + 512UL * 1024UL) / (1024UL * 1024UL)));
    }
}

static bool comes_before(int a, int b)
{
    if (s_is_dir[a] != s_is_dir[b]) {
        return s_is_dir[a] > s_is_dir[b];
    }
    if (s_is_dir[a]) {
        return strcmp(s_names[a], s_names[b]) < 0;
    }
    return strcmp(s_names[a], s_names[b]) > 0;
}

static int load_dir(void)
{
    s_count = 0;
    s_truncated = false;
    if (!app_sd_is_mounted()) {
        return -1;
    }
    app_logger_flush();
    app_fs_lock();
    DIR *dir = opendir(s_dir);
    if (!dir) {
        app_fs_unlock();
        ESP_LOGW(TAG, "opendir %s errno=%d", s_dir, errno);
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (strlen(ent->d_name) >= NAME_LEN) {
            continue;
        }
        if (s_count >= FILE_LIST_MAX) {
            s_truncated = true;
            break;
        }
        char child[160];
        if (!join_path(child, sizeof(child), s_dir, ent->d_name)) {
            continue;
        }
        struct stat st;
        uint8_t is_dir = 0;
        uint32_t bytes = 0;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                is_dir = 1;
            } else if (st.st_size > 0) {
                bytes = st.st_size > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (uint32_t)st.st_size;
            }
        }
        size_t name_len = strlen(ent->d_name);
        memcpy(s_names[s_count], ent->d_name, name_len + 1);
        s_is_dir[s_count] = is_dir;
        s_sizes[s_count] = bytes;
        s_count++;
    }
    closedir(dir);
    app_fs_unlock();

    for (int i = 0; i < s_count; i++) {
        for (int j = i + 1; j < s_count; j++) {
            if (comes_before(j, i)) {
                char tmp[NAME_LEN];
                uint8_t tmp_dir = s_is_dir[i];
                uint32_t tmp_size = s_sizes[i];
                memcpy(tmp, s_names[i], NAME_LEN);
                memcpy(s_names[i], s_names[j], NAME_LEN);
                memcpy(s_names[j], tmp, NAME_LEN);
                s_is_dir[i] = s_is_dir[j];
                s_is_dir[j] = tmp_dir;
                s_sizes[i] = s_sizes[j];
                s_sizes[j] = tmp_size;
            }
        }
    }
    ESP_LOGI(TAG, "dir %s count %d", s_dir, s_count);
    return s_count;
}

static void sanitize(char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            continue;
        }
        if (ch < 0x20 || ch == 0x7F) {
            text[i] = '.';
        }
    }
    text[len] = '\0';
}

static long align_line(FILE *fp, long offset, long size)
{
    if (offset <= 0) {
        return 0;
    }
    if (offset >= size) {
        return size;
    }
    fseek(fp, offset, SEEK_SET);
    int ch = 0;
    int guard = 0;
    while (offset < size && (ch = fgetc(fp)) != EOF && ch != '\n' && guard < 180) {
        offset++;
        guard++;
    }
    if (ch == '\n') {
        offset++;
    }
    return offset;
}

static int read_window(void)
{
    app_fs_lock();
    app_logger_release_files();
    FILE *fp = fopen(s_view_path, "rb");
    if (!fp) {
        int err = errno;
        app_fs_unlock();
        ESP_LOGW(TAG, "open %s errno=%d", s_view_path, err);
        return err == 0 ? -1 : -err;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        app_fs_unlock();
        return -1;
    }
    s_view_size = ftell(fp);
    if (s_view_size < 0) {
        s_view_size = 0;
    }
    long start = s_view_offset;
    if (start < 0 || start > s_view_size) {
        start = s_view_size > VIEW_CAP ? s_view_size - VIEW_CAP : 0;
        start = align_line(fp, start, s_view_size);
        if (start >= s_view_size && s_view_size > 0) {
            start = 0;
        }
    } else if (start > 0) {
        start = align_line(fp, start, s_view_size);
    }
    if (fseek(fp, start, SEEK_SET) != 0) {
        fclose(fp);
        app_fs_unlock();
        return -1;
    }
    size_t got = fread(s_chunk, 1, VIEW_CAP, fp);
    long end = start + (long)got;
    if (end < s_view_size) {
        size_t cut = got;
        while (cut > 0 && s_chunk[cut - 1] != '\n') {
            cut--;
        }
        if (cut > 0) {
            got = cut;
            end = start + (long)got;
        }
    }
    fclose(fp);
    app_fs_unlock();
    sanitize(s_chunk, got);
    s_view_offset = start;
    s_view_end = end;
    return (int)got;
}

static void show_list(void)
{
    s_viewing = false;
    if (s_mid_label) {
        lv_label_set_text(s_mid_label, "刷新");
    }
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_view_box, LV_OBJ_FLAG_HIDDEN);
    if (!app_sd_is_mounted()) {
        lv_label_set_text(s_info, "SD未挂载");
        for (int i = 0; i < FILE_PAGE; i++) {
            lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_count <= 0) {
        lv_label_set_text_fmt(s_info, "%s  无文件", dir_title());
        for (int i = 0; i < FILE_PAGE; i++) {
            lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    int pages = (s_count + FILE_PAGE - 1) / FILE_PAGE;
    if (pages < 1) {
        pages = 1;
    }
    if (s_list_page >= pages) {
        s_list_page = pages - 1;
    }
    if (s_list_page < 0) {
        s_list_page = 0;
    }
    lv_label_set_text_fmt(s_info, "%s  %d%s  %d/%d", dir_title(), s_count, s_truncated ? "+" : "个",
                          s_list_page + 1, pages);
    for (int i = 0; i < FILE_PAGE; i++) {
        int real = s_list_page * FILE_PAGE + i;
        if (real >= s_count) {
            lv_obj_add_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_items[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *label = lv_obj_get_child(s_items[i], 0);
        if (s_is_dir[real]) {
            lv_label_set_text_fmt(label, "[目录] %s", s_names[real]);
        } else {
            char size_text[12];
            format_size(size_text, sizeof(size_text), s_sizes[real]);
            lv_label_set_text_fmt(label, "%s  %s", s_names[real], size_text);
        }
        use_cn_font(label);
    }
}

static void show_view(void)
{
    lv_obj_add_flag(s_list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_view_box, LV_OBJ_FLAG_HIDDEN);
    if (s_mid_label) {
        lv_label_set_text(s_mid_label, "最前");
    }
    const char *name = strrchr(s_view_path, '/');
    lv_label_set_text(s_title, name ? name + 1 : s_view_path);

    int got = read_window();
    if (got < 0) {
        lv_label_set_text_fmt(s_text, "无法打开 %d", -got);
        lv_label_set_text(s_info, s_view_path);
        return;
    }
    if (s_view_size == 0 || got == 0) {
        lv_label_set_text(s_text, s_view_size == 0 ? "文件是空的" : "末尾");
    } else {
        lv_label_set_text(s_text, s_chunk);
    }
    lv_label_set_text_fmt(s_info, "%ld/%ld字节", s_view_offset, s_view_size);
    if (s_scroll) {
        lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
    }
}

static void on_item(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    int real = s_list_page * FILE_PAGE + index;
    if (real < 0 || real >= s_count) {
        return;
    }
    if (s_is_dir[real]) {
        char next[96];
        if (!join_path(next, sizeof(next), s_dir, s_names[real])) {
            lv_label_set_text(s_info, "无法进入");
            return;
        }
        snprintf(s_dir, sizeof(s_dir), "%s", next);
        s_list_page = 0;
        load_dir();
        show_list();
        return;
    }
    if (!join_path(s_view_path, sizeof(s_view_path), s_dir, s_names[real])) {
        lv_label_set_text(s_info, "无法打开");
        return;
    }
    s_viewing = true;
    s_follow_tail = true;
    s_view_offset = -1;
    show_view();
}

static void on_back(lv_event_t *event)
{
    (void)event;
    if (s_viewing) {
        s_viewing = false;
        s_follow_tail = false;
        show_list();
        return;
    }
    if (strcmp(s_dir, APP_SD_MOUNT) != 0) {
        set_parent_dir(s_dir);
        s_list_page = 0;
        load_dir();
    }
    show_list();
}

static void on_mid(lv_event_t *event)
{
    (void)event;
    if (s_viewing) {
        s_follow_tail = false;
        s_view_offset = 0;
        show_view();
        return;
    }
    s_list_page = 0;
    load_dir();
    show_list();
}

static void on_prev(lv_event_t *event)
{
    (void)event;
    if (s_viewing) {
        if (s_view_offset > 0) {
            s_follow_tail = false;
            s_view_offset = s_view_offset > VIEW_CAP ? s_view_offset - VIEW_CAP : 0;
            show_view();
        }
        return;
    }
    if (s_list_page > 0) {
        s_list_page--;
        show_list();
    }
}

static void on_next(lv_event_t *event)
{
    (void)event;
    if (s_viewing) {
        if (s_view_end < s_view_size) {
            s_follow_tail = false;
            s_view_offset = s_view_end;
            show_view();
        }
        return;
    }
    if ((s_list_page + 1) * FILE_PAGE < s_count) {
        s_list_page++;
        show_list();
    }
}

static void on_tail(lv_event_t *event)
{
    (void)event;
    if (!s_viewing) {
        return;
    }
    s_follow_tail = true;
    s_view_offset = -1;
    show_view();
}

static lv_obj_t *make_small_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_height(btn, 28);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_pad_hor(btn, 2, 0);
    lv_obj_set_style_pad_ver(btn, 2, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    use_cn_font(btn);
    use_cn_font(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) {
        *label_out = label;
    }
    return btn;
}

void app_files_show(void)
{
    if (!s_ready) {
        return;
    }
    if (s_viewing) {
        if (s_follow_tail) {
            s_view_offset = -1;
            show_view();
        }
        return;
    }
    load_dir();
    show_list();
}

void app_files_create(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 2, 0);
    lv_obj_set_style_pad_row(parent, 2, 0);
    use_cn_font(parent);

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    make_small_btn(bar, "返回", on_back, NULL);
    make_small_btn(bar, "刷新", on_mid, &s_mid_label);
    make_small_btn(bar, "上页", on_prev, NULL);
    make_small_btn(bar, "下页", on_next, NULL);
    make_small_btn(bar, "末尾", on_tail, NULL);

    s_info = lv_label_create(parent);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_info, lv_pct(100));
    use_cn_font(s_info);

    s_list_box = lv_obj_create(parent);
    lv_obj_set_width(s_list_box, lv_pct(100));
    lv_obj_set_flex_grow(s_list_box, 1);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list_box, 0, 0);
    lv_obj_set_style_pad_row(s_list_box, 2, 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_bg_opa(s_list_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < FILE_PAGE; i++) {
        s_items[i] = lv_btn_create(s_list_box);
        lv_obj_set_width(s_items[i], lv_pct(100));
        lv_obj_set_height(s_items[i], 30);
        lv_obj_set_style_pad_hor(s_items[i], 6, 0);
        lv_obj_t *label = lv_label_create(s_items[i]);
        lv_label_set_text(label, "");
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(96));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        use_cn_font(s_items[i]);
        use_cn_font(label);
        lv_obj_add_event_cb(s_items[i], on_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_view_box = lv_obj_create(parent);
    lv_obj_set_width(s_view_box, lv_pct(100));
    lv_obj_set_flex_grow(s_view_box, 1);
    lv_obj_set_flex_flow(s_view_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_view_box, 0, 0);
    lv_obj_set_style_pad_row(s_view_box, 2, 0);
    lv_obj_set_style_border_width(s_view_box, 0, 0);
    lv_obj_set_style_bg_opa(s_view_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_view_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_view_box, LV_OBJ_FLAG_HIDDEN);
    s_title = lv_label_create(s_view_box);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_title, lv_pct(100));
    use_cn_font(s_title);
    s_scroll = lv_obj_create(s_view_box);
    lv_obj_set_width(s_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_scroll, 1);
    lv_obj_set_style_pad_all(s_scroll, 4, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    s_text = lv_label_create(s_scroll);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text, lv_pct(100));
    use_cn_font(s_text);
    lv_label_set_text(s_text, "");

    snprintf(s_dir, sizeof(s_dir), "%s", APP_SD_MOUNT);
    load_dir();
    show_list();
    s_ready = true;
}
