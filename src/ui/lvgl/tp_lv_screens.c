/*
 * tp_lv_screens.c — how TinyPod looks.
 *
 * 240 x 432 and driven by three buttons, so the shape follows the launcher on
 * the same device: a fixed header, a body, and one line at the bottom saying
 * what the buttons do here. The palette is deliberately the same as Radio+ and
 * the launcher - apps by the same hand on the same device that look like
 * strangers to each other is a worse result than any of them looking plain.
 *
 * The selected row is the only bright thing on a list. With no pointer, the
 * selection IS the cursor, so it is drawn like one and everything else sits
 * back at half strength.
 *
 * Rows are built once and refilled as the list scrolls, rather than one widget
 * per track. A library is thousands of tracks and a screen holds six; building
 * the other few thousand would cost memory and time for something nobody can
 * see.
 */

#include "tp_lv_screens.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define C_BG        0x08090D
#define C_SURFACE   0x101219
#define C_SURFACE_2 0x171B25
#define C_HAIRLINE  0x232937
#define C_TEXT      0xEDEFF4
#define C_TEXT_DIM  0x8B92A0
#define C_TEXT_MUTE 0x545B69

#define C_ACCENT    0x34D399     /* TinyPod's colour on the launcher */
#define C_WARN      0xFB923C

#define F_BIG     (&lv_font_montserrat_24)
#define F_NAME    (&lv_font_montserrat_20)
#define F_BODY    (&lv_font_montserrat_16)
#define F_CAPTION (&lv_font_montserrat_14)

#define MARGIN    12
#define CONTENT_W (TP_LV_W - 2 * MARGIN)

#define HEADER_H  34
#define FOOTER_Y  (TP_LV_H - 26)

#define LIST_TOP  40
#define ROW_H     54
#define ROWS      6              /* 6 * 54 = 324, ends at y = 364 */

#define BAR_X     (TP_LV_W - 5)
#define BAR_W     3

static uint32_t dim(uint32_t c) { return (c >> 1) & 0x7F7F7Fu; }

/* ---- building blocks ------------------------------------------------------ */

static void flat(lv_obj_t *o, uint32_t colour)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    flat(o, colour);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    return l;
}

/*
 * A label that must not wrap. LV_LABEL_LONG_DOT needs a height as well as a
 * width: given only a width it wraps to a second line, and that line lands on
 * whatever is below it.
 */
static lv_obj_t *fitted(lv_obj_t *parent, const char *text,
                        const lv_font_t *font, uint32_t colour,
                        int x, int y, int w, int h)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_size(l, w, h);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *centred(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t colour, int y, int h)
{
    lv_obj_t *l = label(parent, text, font, colour);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(l, CONTENT_W, h);
    lv_obj_set_pos(l, MARGIN, y);
    return l;
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void ms_to_clock(unsigned long ms, char *out, size_t cap)
{
    unsigned long s = ms / 1000;
    unsigned long m = s / 60;

    /* Hours only when there are any: "3:07" reads better than "0:03:07", and
       an hour-long track is rare enough not to shape the common case. */
    if (m >= 60) {
        /* Clamped so the field has a width the compiler can see. `m` is an
           unsigned long, so without this gcc must assume the hours could be
           twenty digits and warns that the result may be truncated - which it
           could be, for a duration no music file will ever have. */
        unsigned long h = m / 60;
        if (h > 99)
            h = 99;
        snprintf(out, cap, "%lu:%02lu:%02lu", h, m % 60, s % 60);
    }
    else
        snprintf(out, cap, "%lu:%02lu", m, s % 60);
}

/* ---- the shared frame ----------------------------------------------------- */

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_count;
static lv_obj_t *s_hint;
static lv_obj_t *s_hold;

/* the list */
static lv_obj_t *s_list;
static lv_obj_t *s_empty;
static lv_obj_t *s_bar_track;
static lv_obj_t *s_bar_thumb;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *edge;
    lv_obj_t *line1;
    lv_obj_t *line2;
    lv_obj_t *badge;
} row_t;

static row_t s_row[ROWS];

/* now playing */
static lv_obj_t *s_now;
static lv_obj_t *s_art;
static lv_obj_t *s_art_glyph;
static lv_obj_t *s_now_title;
static lv_obj_t *s_now_artist;
static lv_obj_t *s_now_album;
static lv_obj_t *s_pb_track;
static lv_obj_t *s_pb_fill;
static lv_obj_t *s_now_pos;
static lv_obj_t *s_now_dur;
static lv_obj_t *s_now_state;
static lv_obj_t *s_now_extra;

/* a plain message */
static lv_obj_t *s_msg;
static lv_obj_t *s_msg_body;

int tp_lv_list_rows(void) { return ROWS; }

static void build_row(row_t *r, int y)
{
    r->root = panel(s_list, 0, y, TP_LV_W, ROW_H, C_BG);
    r->edge = panel(r->root, 0, 0, 3, ROW_H, C_BG);

    /* The badge is right-aligned in a fixed box, so a two-digit duration and
       a four-digit one do not shift the title beside them. */
    r->badge = label(r->root, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_size(r->badge, 48, 18);
    lv_obj_set_style_text_align(r->badge, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r->badge, TP_LV_W - MARGIN - 48, 8);

    r->line1 = fitted(r->root, "", F_BODY, C_TEXT,
                      MARGIN, 7, TP_LV_W - MARGIN * 2 - 54, 22);
    r->line2 = fitted(r->root, "", F_CAPTION, C_TEXT_DIM,
                      MARGIN, 29, TP_LV_W - MARGIN * 2 - 54, 18);

    panel(r->root, MARGIN, ROW_H - 1, CONTENT_W, 1, C_HAIRLINE);
}

static void build_list(void)
{
    int i;

    s_list = panel(s_screen, 0, LIST_TOP, TP_LV_W, ROW_H * ROWS, C_BG);

    for (i = 0; i < ROWS; i++)
        build_row(&s_row[i], i * ROW_H);

    /* After the rows: they are full width and LVGL draws in creation order,
       so a bar made first is a bar painted over. */
    s_bar_track = panel(s_screen, BAR_X, LIST_TOP, BAR_W, ROW_H * ROWS,
                        C_SURFACE);
    s_bar_thumb = panel(s_screen, BAR_X, LIST_TOP, BAR_W, 20, C_TEXT_MUTE);

    s_empty = centred(s_screen, "", F_BODY, C_TEXT_MUTE, 180, 60);
}

static void build_now(void)
{
    char t[8];

    s_now = panel(s_screen, 0, LIST_TOP, TP_LV_W, TP_LV_H - LIST_TOP - 34, C_BG);

    /* Art placeholder. Square and centred, because that is the shape real art
       will be and the layout should not move when it arrives. */
    s_art = panel(s_now, (TP_LV_W - 128) / 2, 6, 128, 128, C_SURFACE);
    lv_obj_set_style_radius(s_art, 8, 0);
    s_art_glyph = label(s_art, LV_SYMBOL_AUDIO, F_BIG, C_TEXT_MUTE);
    lv_obj_center(s_art_glyph);

    s_now_title  = centred(s_now, "", F_NAME,    C_TEXT,     148, 26);
    lv_label_set_long_mode(s_now_title, LV_LABEL_LONG_DOT);
    s_now_artist = centred(s_now, "", F_BODY,    C_TEXT_DIM, 176, 22);
    lv_label_set_long_mode(s_now_artist, LV_LABEL_LONG_DOT);
    s_now_album  = centred(s_now, "", F_CAPTION, C_TEXT_MUTE, 200, 18);
    lv_label_set_long_mode(s_now_album, LV_LABEL_LONG_DOT);

    s_pb_track = panel(s_now, MARGIN, 232, CONTENT_W, 4, C_SURFACE_2);
    s_pb_fill  = panel(s_now, MARGIN, 232, 1, 4, C_ACCENT);

    s_now_pos = label(s_now, "0:00", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_pos(s_now_pos, MARGIN, 242);

    s_now_dur = label(s_now, "0:00", F_CAPTION, C_TEXT_DIM);
    lv_obj_set_size(s_now_dur, 60, 18);
    lv_obj_set_style_text_align(s_now_dur, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_now_dur, TP_LV_W - MARGIN - 60, 242);

    s_now_state = centred(s_now, "", F_NAME, C_ACCENT, 272, 28);
    s_now_extra = centred(s_now, "", F_CAPTION, C_TEXT_MUTE, 304, 18);

    (void)t;
}

void tp_lv_screens_init(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d)
        lv_theme_default_init(d, lv_color_hex(C_ACCENT),
                              lv_color_hex(C_TEXT_DIM), true, F_BODY);

    s_screen = lv_obj_create(NULL);
    flat(s_screen, C_BG);
    lv_obj_set_size(s_screen, TP_LV_W, TP_LV_H);

    s_title = fitted(s_screen, "TinyPod", F_BODY, C_TEXT, MARGIN, 8, 150, 22);

    s_count = label(s_screen, "", F_CAPTION, C_TEXT_MUTE);
    lv_obj_set_size(s_count, 80, 18);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_count, TP_LV_W - MARGIN - 80, 11);

    panel(s_screen, MARGIN, HEADER_H, CONTENT_W, 1, C_HAIRLINE);

    build_list();
    build_now();

    s_msg = panel(s_screen, 0, LIST_TOP, TP_LV_W, 200, C_BG);
    s_msg_body = centred(s_msg, "", F_CAPTION, C_TEXT_DIM, 60, 120);

    /* Two lines' worth. The longest hint - Now Playing, which has to name
       three different actions - does not fit on one at this width, and
       trimming the words to make it fit was how the Settings hint ended up
       reading "hold PLAY" with the "back" cut off. */
    s_hint = centred(s_screen, "", F_CAPTION, C_TEXT_MUTE, FOOTER_Y - 14, 32);

    /* Fills across the bottom while PLAY is held, so a long press shows it is
       being counted rather than ignored. */
    s_hold = panel(s_screen, 0, TP_LV_H - 2, TP_LV_W, 2, C_ACCENT);
    show(s_hold, false);

    lv_screen_load(s_screen);
}

void tp_lv_set_hint(const char *hint)
{
    lv_label_set_text(s_hint, hint ? hint : "");
}

void tp_lv_set_holding(bool on)
{
    show(s_hold, on);
}

/* ---- the list ------------------------------------------------------------- */

static void fill_row(row_t *r, const struct tp_lv_row *src, bool sel)
{
    lv_obj_set_style_bg_color(r->root, lv_color_hex(sel ? C_SURFACE : C_BG), 0);
    lv_obj_set_style_bg_color(r->edge,
        lv_color_hex(src->playing ? C_ACCENT
                                  : (sel ? dim(C_ACCENT) : C_BG)), 0);

    lv_label_set_text(r->line1, src->line1 ? src->line1 : "");
    lv_obj_set_style_text_color(r->line1,
        lv_color_hex(sel ? C_TEXT : C_TEXT_DIM), 0);

    lv_label_set_text(r->line2, src->line2 ? src->line2 : "");
    lv_obj_set_style_text_color(r->line2,
        lv_color_hex(sel ? C_TEXT_DIM : C_TEXT_MUTE), 0);

    lv_label_set_text(r->badge, src->badge ? src->badge : "");
    lv_obj_set_style_text_color(r->badge,
        lv_color_hex(src->playing ? C_ACCENT : C_TEXT_MUTE), 0);

    /* A row with nothing on its second line gets the first one centred rather
       than sitting high with a gap under it. */
    if (src->line2 && src->line2[0]) {
        lv_obj_set_pos(r->line1, MARGIN, 7);
        show(r->line2, true);
    } else {
        lv_obj_set_pos(r->line1, MARGIN, 16);
        show(r->line2, false);
    }
}

void tp_lv_show_list(const char *title, int count, int sel, int top,
                     void (*fill)(int index, struct tp_lv_row *out, void *ctx),
                     void *ctx, const char *empty)
{
    char t[24];
    int i;

    lv_label_set_text(s_title, title ? title : "");
    show(s_now, false);
    show(s_msg, false);
    show(s_list, count > 0);

    if (count <= 0) {
        lv_label_set_text(s_count, "");
        lv_label_set_text(s_empty, empty ? empty : "Nothing here");
        show(s_empty, true);
        show(s_bar_track, false);
        show(s_bar_thumb, false);
        return;
    }
    show(s_empty, false);

    snprintf(t, sizeof t, "%d/%d", sel + 1, count);
    lv_label_set_text(s_count, t);

    for (i = 0; i < ROWS; i++) {
        struct tp_lv_row row;
        int idx = top + i;

        if (idx >= count) {
            show(s_row[i].root, false);
            continue;
        }
        show(s_row[i].root, true);

        memset(&row, 0, sizeof row);
        fill(idx, &row, ctx);
        fill_row(&s_row[i], &row, idx == sel);
    }

    /* The indicator only appears when there is something off screen. */
    if (count > ROWS) {
        int track = ROW_H * ROWS;
        int h = track * ROWS / count;
        int span = count - ROWS;

        if (h < 20) h = 20;
        show(s_bar_track, true);
        show(s_bar_thumb, true);
        lv_obj_set_pos(s_bar_thumb, BAR_X, LIST_TOP + (track - h) * top / span);
        lv_obj_set_size(s_bar_thumb, BAR_W, h);
    } else {
        show(s_bar_track, false);
        show(s_bar_thumb, false);
    }
}

/* ---- now playing ---------------------------------------------------------- */

void tp_lv_show_now(const struct tp_lv_now *n)
{
    char a[16], b[16], t[48];

    lv_label_set_text(s_title, "Now Playing");
    show(s_list, false);
    show(s_empty, false);
    show(s_bar_track, false);
    show(s_bar_thumb, false);
    show(s_msg, false);
    show(s_now, true);

    if (n->total > 0) {
        snprintf(t, sizeof t, "%d/%d", n->index, n->total);
        lv_label_set_text(s_count, t);
    } else {
        lv_label_set_text(s_count, "");
    }

    lv_label_set_text(s_now_title,  n->title  ? n->title  : "Nothing playing");
    lv_label_set_text(s_now_artist, n->artist ? n->artist : "");
    lv_label_set_text(s_now_album,  n->album  ? n->album  : "");

    ms_to_clock(n->pos_ms, a, sizeof a);
    ms_to_clock(n->dur_ms, b, sizeof b);
    lv_label_set_text(s_now_pos, a);
    lv_label_set_text(s_now_dur, b);

    /*
     * Width from the ratio, clamped. A duration of zero means the decoder has
     * not reported one yet, and dividing by it would be the last thing this
     * process did.
     */
    if (n->dur_ms > 0) {
        unsigned long w = (unsigned long)CONTENT_W * n->pos_ms / n->dur_ms;
        if (w < 1) w = 1;
        if (w > (unsigned long)CONTENT_W) w = CONTENT_W;
        lv_obj_set_size(s_pb_fill, (int)w, 4);
        show(s_pb_fill, true);
    } else {
        show(s_pb_fill, false);
    }

    if (n->error && n->error[0]) {
        lv_label_set_text(s_now_state, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(s_now_state, lv_color_hex(C_WARN), 0);
        lv_label_set_text(s_now_extra, n->error);
        return;
    }

    switch (n->state) {
    case 1:
        lv_label_set_text(s_now_state, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(s_now_state, lv_color_hex(C_ACCENT), 0);
        break;
    case 2:
        lv_label_set_text(s_now_state, LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(s_now_state, lv_color_hex(C_TEXT_DIM), 0);
        break;
    default:
        lv_label_set_text(s_now_state, LV_SYMBOL_STOP);
        lv_obj_set_style_text_color(s_now_state, lv_color_hex(C_TEXT_MUTE), 0);
        break;
    }

    t[0] = 0;
    if (n->shuffle)
        snprintf(t, sizeof t, "%s  shuffle", n->codec ? n->codec : "");
    else if (n->codec)
        snprintf(t, sizeof t, "%s", n->codec);
    lv_label_set_text(s_now_extra, t);
}

/* ---- a message ------------------------------------------------------------ */

/*
 * The scan screen, and the only thing on the display while the library loads.
 *
 * The load used to happen in main() before this UI existed, so a device with
 * five hundred tracks sat on whatever the launcher had left on the panel for
 * the whole of it - which looks exactly like an app that failed to start.
 *
 * Built on the message screen rather than as a new one: it is a title, a line
 * of detail and a bar, and all three already exist here.
 */
void tp_lv_show_scan(const char *where, const char *stage, int pct)
{
    char body[256];

    lv_label_set_text(s_title, "Scanning");
    lv_label_set_text(s_count, "");
    show(s_list, false);
    show(s_now, false);
    show(s_empty, false);
    show(s_msg, true);

    snprintf(body, sizeof body, "%s\n\n%s",
             where && where[0] ? where : "", stage ? stage : "");
    lv_label_set_text(s_msg_body, body);

    /* The transport bar doubles as the progress bar. A negative percentage
       means the stage cannot say how far along it is, and then the bar is
       hidden rather than shown full or empty - both of which would be a
       claim. */
    if (pct < 0) {
        show(s_bar_track, false);
        show(s_bar_thumb, false);
    } else {
        if (pct > 100) pct = 100;
        show(s_bar_track, true);
        show(s_bar_thumb, true);
        lv_obj_set_width(s_bar_thumb, (TP_LV_W - 24) * pct / 100);
    }

    /* Drawn now, not next frame: the caller is about to block. */
    lv_refr_now(NULL);
}

void tp_lv_show_message(const char *title, const char *body)
{
    lv_label_set_text(s_title, title ? title : "");
    lv_label_set_text(s_count, "");
    show(s_list, false);
    show(s_now, false);
    show(s_empty, false);
    show(s_bar_track, false);
    show(s_bar_thumb, false);
    show(s_msg, true);
    lv_label_set_text(s_msg_body, body ? body : "");
}
