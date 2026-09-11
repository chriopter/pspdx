/*
 * The browser: a list of names on the left, the selected package on the
 * right, both standing on the lattice. Layout lives here; everything that
 * moves on its own lives in lattice.c; the room takes a new colour with
 * every selection, drawn by lot.
 *
 * Nothing here holds state the catalog already has. What lives across frames
 * is motion -- the eased selection, the theme mid-crossfade -- and the one
 * screenshot texture.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gui/shell.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/icons.h"
#include "gui/lattice.h"
#include "gui/title.h"
#include "gui/palette.h"
#include "gui/preview.h"
#include "util/runtime.h"

/* Three type roles and nowhere else a fourth: FONT_H1 for the one name on
   screen, FONT_BODY for the list and the summary, FONT_META for facts. */

#define HEADER_H 32
#define FOOTER_Y 252

#define LIST_X 16
#define LIST_W 200
#define LIST_Y 44
#define ITEM_H 32
/* The bundle's icon, 144x80 shown at a third: as tall as the row allows
   with a little air, and the name starts after it. */
#define ICON_W 43
#define ICON_H 24
#define NAME_X (LIST_X + ICON_W + 9)
#define VISIBLE ((FOOTER_Y - 6 - LIST_Y) / ITEM_H)

#define PANEL_X 232
#define SHOT_W 224
#define SHOT_H (SHOT_W * SCR_H / SCR_W)     /* the screen's own 480:272 */
#define SHOT_Y 40
#define REFLECT_H 18

/* ------------------------------------------------------------------ colour */

static const struct rgb NIGHT_TOP = { 2, 3, 9 };
static const struct rgb NIGHT_BOTTOM = { 6, 8, 22 };

static const struct rgb DEFAULT_TINT = { 80, 140, 255 };

/* Every selection lights the room a colour drawn by lot: a hue at full
   saturation, always at least a third of the wheel from the last one, so
   the change is a change. The lot is a plain generator seeded by the clock
   and has nothing to do with the entropy pool. */
static unsigned g_lot;
static float g_hue = 0.58f;

static struct rgb hue_rgb(float h) {
    h -= (float)(int)h;
    float x = h * 6.0f;
    int sector = (int)x;
    float f = x - sector;
    int up = (int)(f * 255.0f), down = 255 - up;
    switch (sector) {
    case 0: return (struct rgb){ 255, up, 0 };
    case 1: return (struct rgb){ down, 255, 0 };
    case 2: return (struct rgb){ 0, 255, up };
    case 3: return (struct rgb){ 0, down, 255 };
    case 4: return (struct rgb){ up, 0, 255 };
    default: return (struct rgb){ 255, 0, down };
    }
}

static struct rgb draw_lot(void) {
    if (!g_lot) g_lot = now_us() | 1;
    g_lot = g_lot * 1664525u + 1013904223u;
    /* A third to two thirds of the wheel away, either direction. */
    float step = 0.33f + 0.34f * ((g_lot >> 8) & 0xFFFF) / 65536.0f;
    g_hue += step;
    g_hue -= (float)(int)g_hue;
    /* Not the yellows: water lit yellow is mud. The band from orange-yellow
       to yellow-green is stepped over. */
    if (g_hue > 0.10f && g_hue < 0.22f) g_hue += 0.12f;
    /* Softened a little: pure spectral colours read as a warning light. */
    return rgb_mix(hue_rgb(g_hue), RGB_WHITE, 0.18f);
}

/* The palette of the current frame, derived from the eased tint once per
   frame so every element agrees. */
static struct rgb g_tint = { 80, 140, 255 };
static unsigned g_accent, g_text, g_dim;

static void derive_palette(void) {
    g_accent = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.45f), 255);
    g_text = rgb_pack(rgb_mix(RGB_WHITE, g_tint, 0.05f), 255);
    g_dim = rgb_pack(rgb_mix(rgb_mix(NIGHT_BOTTOM, RGB_WHITE, 0.58f), g_tint, 0.12f), 255);
}

/* ------------------------------------------------------------------- state */

static int g_last_cursor = -1;
static float g_sel_y = LIST_Y;
static int g_first;
static int g_fade = 255;                /* black over everything at start */

/* Install overlay, live only between shell_install_begin and _end. */
static int g_installing;
static char g_install_name[40];
static char g_install_phase[16];
static size_t g_install_done, g_install_total;
static unsigned g_install_drawn_ms;
static char g_status[96];
static char g_word[24] = "Connecting";
static const struct catalog *g_catalog;
static int g_cursor;

/* --------------------------------------------------------------- the view */

/* The tabs, in the order they are shown. The first takes everything; the
   rest match the catalog's own lowercase category word. */
static const char *const TAB_NAME[] = {
    "All", "Games", "Demos", "Apps", "Emulators", "Plugins"
};
static const char *const TAB_KEY[] = {
    "", "games", "demos", "apps", "emulators", "plugins"
};
#define TAB_ALL 6

static int g_tab[TAB_ALL];              /* which of the six have anything */
static int g_tabs;
static int g_tab_at;                    /* index into g_tab, not into TAB_NAME */
static const struct catalog *g_view_of;
static unsigned char g_view[MAX_APPS];
static int g_view_count;

static void build_view(void) {
    g_view_count = 0;
    if (!g_view_of || g_tabs <= 0) return;
    const char *key = TAB_KEY[g_tab[g_tab_at]];
    for (int i = 0; i < g_view_of->count; i++)
        if (!key[0] || strcmp(g_view_of->apps[i].category, key) == 0)
            g_view[g_view_count++] = (unsigned char)i;
    /* The list and the card both start again from the top of what is now
       shown, and the card is told to fetch afresh: the row a cursor names
       is a different package than it was a moment ago. */
    g_first = 0;
    g_last_cursor = -1;
}

void shell_view_rebuild(const struct catalog *catalog) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    g_view_of = catalog;
    g_tabs = 0;
    g_tab_at = 0;
    if (!catalog || catalog->count <= 0) { g_view_count = 0; return; }
    for (int t = 0; t < TAB_ALL; t++) {
        int has = !TAB_KEY[t][0];
        for (int i = 0; !has && i < catalog->count; i++)
            has = strcmp(catalog->apps[i].category, TAB_KEY[t]) == 0;
        if (!has) continue;
        if (t == was) g_tab_at = g_tabs;
        g_tab[g_tabs++] = t;
    }
    build_view();
}

int shell_view_count(void) { return g_view_count; }

int shell_view_index(int row) {
    return row >= 0 && row < g_view_count ? g_view[row] : -1;
}

int shell_view_row(int index) {
    for (int row = 0; row < g_view_count; row++)
        if (g_view[row] == index) return row;
    return -1;
}

int shell_tab_count(void) { return g_tabs; }

void shell_tab_move(int step) {
    if (g_tabs <= 1) return;
    g_tab_at = (g_tab_at + step + g_tabs) % g_tabs;
    build_view();
}

int shell_init(void) {
    if (!font_init()) return 0;
    gfx_init();
    lattice_init();
    preview_init();
    return 1;
}

void shell_shutdown(void) {
    preview_shutdown();
    icons_reset();
    font_shutdown();
    gfx_shutdown();
}

/* ------------------------------------------------------------------ chrome */

/* Under a block of text the floor is dimmed, but with a soft spot and not
   a plate: the lattice runs everywhere, it only gets quieter here. */
static void draw_shade(int cx, int cy, int w, int h) {
    gfx_shade(cx, cy, w * 1.6f, h * 1.8f, 170);
}

/* The tabs sit between the name and the count, spread across whatever room
   the two of them leave. The active one is lit rather than boxed: a word in
   the text colour with the room's own light welling up under it. */
static void draw_tabs(float left, float right) {
    if (g_tabs <= 1) return;
    float words = 0;
    for (int i = 0; i < g_tabs; i++) words += font_width(FONT_META, TAB_NAME[g_tab[i]]);
    float gap = (right - left - words) / (g_tabs - 1);
    if (gap > 22) gap = 22;
    if (gap < 7) gap = 7;
    float x = left + (right - left - words - gap * (g_tabs - 1)) / 2;
    if (x < left) x = left;
    for (int i = 0; i < g_tabs; i++) {
        const char *name = TAB_NAME[g_tab[i]];
        float w = font_width(FONT_META, name);
        if (i == g_tab_at) {
            gfx_glow(x + w / 2, 17, w + 30, 30, rgb_pack(g_tint, 110));
            gfx_glow(x + w / 2, 24, w + 8, 7,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.6f), 120));
        }
        font_print(FONT_META, x, 21, i == g_tab_at ? g_text : g_dim, name);
        x += w + gap;
    }
}

static void draw_chrome(const struct catalog *catalog) {
    gfx_vgrad(0, 0, SCR_W, HEADER_H, RGBA(255, 255, 255, 14), RGBA(255, 255, 255, 0));
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), 150);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(0, HEADER_H, SCR_W / 2, 1, clear, bright);
    gfx_hgrad(SCR_W / 2, HEADER_H, SCR_W / 2, 1, bright, clear);
    gfx_hgrad(0, FOOTER_Y, SCR_W / 2, 1, clear, rgb_pack(g_tint, 110));
    gfx_hgrad(SCR_W / 2, FOOTER_Y, SCR_W / 2, 1, rgb_pack(g_tint, 110), clear);

    gfx_glow(LIST_X + 24, 18, 110, 56, rgb_pack(g_tint, 80));
    font_print(FONT_H1, LIST_X, 23, g_text, "PSPDX");

    /* The count is the count of what is shown: the whole catalog under All,
       the tab's own word under any other. It changes when the catalog or
       the tab does, which is not every frame. */
    static char right[64];
    static int said_total = -1, said_updates = -1, said_tab = -1;
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    int shown = tab ? g_view_count : catalog->total;
    int updates = 0;
    for (int row = 0; row < g_view_count; row++)
        if (catalog->apps[g_view[row]].state == APP_UPDATE) updates++;
    if (shown != said_total || updates != said_updates || tab != said_tab) {
        said_total = shown;
        said_updates = updates;
        said_tab = tab;
        const char *what = tab ? TAB_KEY[tab] : "apps";
        if (updates)
            snprintf(right, sizeof(right), "%d %s  %d update%s", shown, what,
                     updates, updates == 1 ? "" : "s");
        else
            snprintf(right, sizeof(right), "%d %s", shown, what);
    }
    if (catalog->count > 0) {
        float x = SCR_W - LIST_X - font_width(FONT_META, right);
        font_print(FONT_META, x, 21, g_dim, right);
        draw_tabs(LIST_X + font_width(FONT_H1, "PSPDX") + 22, x - 16);
    }
}

/* ------------------------------------------------------------------- list */

/* One word for where a package stands, and the colour it stands in. The two
   that name a version are built once per selection: the version they quote
   is written by the update check or by an install, both of which move the
   state with it. */
static const char *state_word(const struct app_entry *entry, unsigned *color) {
    static char word[48];
    static const struct app_entry *said;
    static enum app_state said_state;
    int fresh = entry != said || entry->state != said_state;
    said = entry;
    said_state = entry->state;
    switch (entry->state) {
    case APP_UPDATE:
        if (fresh)
            snprintf(word, sizeof(word), "update to %s", entry->remote_version);
        *color = RGB(140, 255, 170);
        return word;
    case APP_UNKNOWN:
        if (fresh)
            snprintf(word, sizeof(word), "installed %s", entry->local_version);
        *color = g_dim;
        return word;
    case APP_CURRENT:
        *color = g_dim;
        return "up to date";
    case APP_NOT_INSTALLED:
    default:
        *color = g_dim;
        return "not installed";
    }
}

/* The same colour, quieter: a mark on a row the eye is not on should be
   read only when it is looked for. */
static unsigned faded(unsigned color, int alpha) {
    return (color & 0x00FFFFFFu) | ((unsigned)alpha << 24);
}

/* A tick, the way a list is ticked: two strokes, the short one down to the
   corner and the long one up from it. What an installed package gets --
   eight pixels of hairline, which on this screen is a mark and not a
   badge. */
static void draw_tick(float cx, float cy, unsigned color) {
    float x[3] = { cx - 4.0f, cx - 1.3f, cx + 4.0f };
    float y[3] = { cy + 0.4f, cy + 3.1f, cy - 3.2f };
    unsigned c[3] = { color, color, color };
    gfx_ribbon(x, y, c, 3, 0.6f);
}

/* The system's own sign for an update: two arrows chasing each other
   round a circle. Each is an arc of a little under a half turn with a
   head on its leading end. */
static void draw_update_arrows(float cx, float cy, unsigned color, float t) {
    const float r = 4.0f;
    float spin = t * 1.2f;
    for (int arrow = 0; arrow < 2; arrow++) {
        float x[8], y[8];
        unsigned c[8];
        float a0 = spin + arrow * 3.1416f;
        for (int i = 0; i < 6; i++) {
            float a = a0 + i * (2.4f / 5);
            x[i] = cx + cosf(a) * r;
            y[i] = cy + sinf(a) * r;
            c[i] = color;
        }
        gfx_ribbon(x, y, c, 6, 0.7f);
        /* The head: a short stroke to either side of the arc's end, laid
           back along it. */
        float a = a0 + 2.4f, tx = -sinf(a), ty = cosf(a);
        float ex = x[5], ey = y[5];
        float hx[3] = { ex - tx * 2.3f - cosf(a) * 1.9f, ex, ex - tx * 2.3f + cosf(a) * 1.9f };
        float hy[3] = { ey - ty * 2.3f - sinf(a) * 1.9f, ey, ey - ty * 2.3f + sinf(a) * 1.9f };
        gfx_ribbon(hx, hy, c, 3, 0.7f);
    }
}

static void draw_list(const struct catalog *catalog, int cursor, float t) {
    if (cursor < g_first) g_first = cursor;
    if (cursor >= g_first + VISIBLE) g_first = cursor - VISIBLE + 1;
    if (g_first < 0) g_first = 0;

    /* The bar chases the selection rather than jumping to it. A quarter of
       the remaining distance per frame settles in about a fifth of a second
       and never overshoots. */
    float target = LIST_Y + (cursor - g_first) * ITEM_H;
    g_sel_y += (target - g_sel_y) * 0.25f;

    int rows = g_view_count < VISIBLE ? g_view_count : VISIBLE;
    draw_shade(LIST_X + LIST_W / 2, LIST_Y + rows * ITEM_H / 2, LIST_W, rows * ITEM_H);

    /* The selected row glows: a breathing light behind it and a thin
       streak of light under it, nothing with a corner. */
    float breathe = 0.85f + 0.15f * sinf(t * 2.2f);
    float mid = g_sel_y + ITEM_H / 2 - 1;
    gfx_glow(LIST_X + 60, mid, LIST_W + 170, ITEM_H * 3.4f,
             rgb_pack(g_tint, (int)(130 * breathe)));
    gfx_glow(LIST_X + 40, mid, LIST_W + 40, ITEM_H * 1.2f,
             rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.5f), (int)(70 * breathe)));
    gfx_glow(LIST_X + LIST_W / 2, g_sel_y + ITEM_H - 3, LIST_W + 30, 10,
             rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 160));

    /* The icons module counts in catalog entries, so the rows on screen are
       handed over as the entries they stand for. */
    int wanted[VISIBLE], want_count = 0;
    for (int i = g_first; i < g_view_count && i < g_first + VISIBLE; i++)
        wanted[want_count++] = g_view[i];
    icons_bind(catalog);
    if (icons_want(wanted, want_count)) preview_poke();

    for (int i = g_first; i < g_view_count && i < g_first + VISIBLE; i++) {
        const struct app_entry *entry = &catalog->apps[g_view[i]];
        int y = LIST_Y + (i - g_first) * ITEM_H;
        int selected = i == cursor;

        /* The bundle's own icon, dimmed with the name; a dark plate where
           it has not arrived, so the column reads as a column. */
        const struct gfx_texture *icon = icons_get(g_view[i]);
        int iy = y + (ITEM_H - ICON_H) / 2;
        if (icon)
            gfx_texture_draw(icon, LIST_X, iy, ICON_W, ICON_H,
                             selected ? RGB(255, 255, 255) : RGB(150, 150, 150));
        else
            gfx_rect(LIST_X, iy, ICON_W, ICON_H, RGBA(255, 255, 255, selected ? 24 : 12));

        /* A mark, not a word, at the end of the row: the line ticked off
           when the package is on the stick, the system's turning arrows
           when a newer one waits, nothing for the rest. */
        float mx = LIST_X + LIST_W - 8, my = y + ITEM_H / 2 - 1;
        int name_w = LIST_X + LIST_W - NAME_X;
        if (entry->state == APP_UPDATE) {
            gfx_glow(mx, my, 26, 26, RGBA(140, 255, 170, selected ? 130 : 60));
            draw_update_arrows(mx, my, faded(RGB(170, 255, 190), selected ? 255 : 153), t);
            name_w -= 18;
        } else if (entry->state != APP_NOT_INSTALLED) {
            draw_tick(mx, my, selected ? g_accent : faded(g_dim, 153));
            name_w -= 18;
        }

        font_print_clipped(FONT_BODY, NAME_X, y + 21, name_w,
                           selected ? g_text : g_dim, entry->name);
    }

    if (g_view_count > VISIBLE) {
        int track = FOOTER_Y - 6 - LIST_Y;
        int knob = track * VISIBLE / g_view_count;
        int at = track * g_first / g_view_count;
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y, 2, track, RGBA(255, 255, 255, 24));
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y + at, 2, knob, g_accent);
    }
}

/* ------------------------------------------------------------------ panel */

static void draw_panel(const struct app_entry *entry, float t) {
    /* The card stands still: a picture that drifts is a picture that is
       hard to look at. What moves is the light over it. */
    struct gfx_card card;
    card.cx = PANEL_X + SHOT_W / 2;
    card.cy = SHOT_Y + SHOT_H / 2;
    card.w = SHOT_W;
    card.h = SHOT_H;
    card.yaw = 0.0f;
    card.pitch = 0.0f;
    card.reflect_h = REFLECT_H + 6;
    /* One sweep of light every twelve seconds, taking two of them. */
    float cycle = fmodf(t, 12.0f);
    card.gloss = cycle < 2.0f ? cycle / 2.0f : -1.0f;

    /* Backlit: the light sits behind the picture and leaks out around it. */
    gfx_glow(card.cx, card.cy, SHOT_W + 130, SHOT_H + 120, rgb_pack(g_tint, 100));

    int still_alpha, film_alpha;
    const struct gfx_texture *still = preview_still(&still_alpha);
    const struct gfx_texture *film = preview_film(&film_alpha);
    enum preview_state picture = preview_state();
    if (still || film) {
        /* The still first, the film fading in over it. */
        if (still && !(film && film_alpha >= 250)) {
            card.alpha = still_alpha;
            gfx_card_draw(still, &card);
        }
        if (film) {
            card.alpha = film_alpha;
            gfx_card_draw(film, &card);
        }
    } else {
        card.alpha = 255;
        gfx_card_draw(0, &card);
        if (picture == PREVIEW_LOADING)
            gfx_glow(card.cx, card.cy, 90 + sinf(t * 4) * 20, 50 + sinf(t * 4) * 12,
                     rgb_pack(g_tint, 120));
        const char *note = picture == PREVIEW_LOADING ? "loading"
                         : picture == PREVIEW_MISSING ? "no picture" : "";
        font_print(FONT_META, card.cx - font_width(FONT_META, note) / 2,
                   card.cy + 4, g_dim, note);
    }

    /* Under the picture, in reading order: what it is called, what it is,
       the facts about it. */
    int y = SHOT_Y + SHOT_H + REFLECT_H + 16;
    draw_shade(PANEL_X + SHOT_W / 2, y + 12, SHOT_W, 60);
    font_print_clipped(FONT_H1, PANEL_X, y, SHOT_W, g_text, entry->name);
    font_print_clipped(FONT_BODY, PANEL_X, y + 20, SHOT_W, g_dim, entry->summary);

    unsigned state_color;
    const char *state = state_word(entry, &state_color);
    /* Category, licence and author are read once from the catalog and never
       change again, so the line only has to be built when the selection
       moves. */
    static char facts[128];
    static const struct app_entry *facts_of;
    if (entry != facts_of) {
        facts_of = entry;
        snprintf(facts, sizeof(facts), "%s   %s   %s", entry->category,
                 entry->license, entry->author);
    }
    float x = font_print_clipped(FONT_META, PANEL_X, y + 38, SHOT_W, g_dim, facts);
    font_print_clipped(FONT_META, x + 12, y + 38, PANEL_X + SHOT_W - x - 12,
                       state_color, state);
}

/* --------------------------------------------------------------- overlays */

static void draw_install(void) {
    int box_y = 96;
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 120));
    gfx_glow(SCR_W / 2, box_y + 40, 560, 200, rgb_pack(g_tint, 80));
    gfx_rect(0, box_y, SCR_W, 80, RGBA(0, 0, 0, 170));
    gfx_hgrad(0, box_y, SCR_W, 1, rgb_pack(g_tint, 0), rgb_pack(g_tint, 200));
    gfx_hgrad(0, box_y + 80, SCR_W, 1, rgb_pack(g_tint, 200), rgb_pack(g_tint, 0));

    font_print_clipped(FONT_H1, 30, box_y + 30, SCR_W - 60, g_text, g_install_name);
    font_print(FONT_META, 30, box_y + 50, g_accent, g_install_phase);

    int bar_x = 30, bar_w = SCR_W - 60, bar_y = box_y + 60;
    gfx_rect(bar_x, bar_y, bar_w, 6, RGBA(255, 255, 255, 30));
    if (g_install_total) {
        int filled = (int)((unsigned long long)g_install_done * bar_w / g_install_total);
        gfx_hgrad(bar_x, bar_y, filled, 6, rgb_pack(g_tint, 255), g_accent);
        gfx_glow(bar_x + filled, bar_y + 3, 40, 24, rgb_pack(RGB_WHITE, 160));
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%",
                 (int)((unsigned long long)g_install_done * 100 / g_install_total));
        font_print(FONT_META, SCR_W - 30 - font_width(FONT_META, pct), box_y + 50,
                   g_dim, pct);
    } else if (g_install_done) {
        /* No content-length: show that bytes are moving, not how far. */
        int slide = (int)(gfx_frames() * 3 % (unsigned)bar_w);
        int w = 40 > bar_w - slide ? bar_w - slide : 40;
        gfx_hgrad(bar_x + slide, bar_y, w, 6, g_accent, rgb_pack(g_tint, 0));
    }
}

static void draw_footer(void) {
    gfx_vgrad(0, FOOTER_Y + 1, SCR_W, SCR_H - FOOTER_Y - 1, RGBA(0, 0, 0, 110),
              RGBA(0, 0, 0, 190));
    if (g_catalog && g_catalog->count <= 0 && g_status[0]) return;   /* said in the middle */
    if (g_status[0]) {
        font_print_clipped(FONT_META, LIST_X, FOOTER_Y + 15, SCR_W - 2 * LIST_X,
                           g_accent, g_status);
        return;
    }
    /* The keys, and only the keys that do anything: the triggers are worth
       naming once there is a second tab to reach with them. */
    static char keys[96];
    static int said_tabs = -1;
    if (g_tabs != said_tabs) {
        said_tabs = g_tabs;
        snprintf(keys, sizeof(keys), "X install   %sSELECT refresh   HOME quit",
                 g_tabs > 1 ? "L R category   " : "");
    }
    font_print(FONT_META, LIST_X, FOOTER_Y + 15, g_dim,
               g_installing ? "installing, do not turn off" : keys);
}

/* ------------------------------------------------------------------ frame */

/* Where the slowest frame of a window spent its time, in microseconds:
   the backdrop, the text and cards, the sync with the GE plus the wait
   for vblank. Read and reset by shell_profile(). */
static unsigned g_worst_total, g_worst_back, g_worst_front, g_worst_end;

void shell_profile(char *out, int size) {
    unsigned ge, vblank;
    gfx_frame_worst(&ge, &vblank);
    snprintf(out, size, "slowest draw %u us: back %u, front %u, end %u (ge %u, vblank %u)",
             g_worst_total, g_worst_back, g_worst_front, g_worst_end, ge, vblank);
    g_worst_total = g_worst_back = g_worst_front = g_worst_end = 0;
}

void shell_draw(const struct catalog *catalog, int cursor) {
    g_catalog = catalog;
    g_cursor = cursor;
    float t = gfx_frames() * (1.0f / 60.0f);
    unsigned t0 = now_us();

    /* The room changes colour with the selection, but slowly: an eighth of
       the way per frame is a crossfade, not a flash. */
    static struct rgb target = { 80, 140, 255 };
    static int lit_for = -1;
    if (catalog->count <= 0) target = DEFAULT_TINT;
    else if (cursor != lit_for) { lit_for = cursor; target = draw_lot(); }
    g_tint = rgb_mix(g_tint, target, 0.12f);
    derive_palette();

    /* The word for the wait is baked between frames, once per word. */
    if (catalog->count <= 0 && g_status[0]) title_prepare(g_word, g_tint);

    gfx_frame_begin(0xFF000000);
    gfx_vgrad(0, 0, SCR_W, SCR_H, rgb_pack(rgb_mix(NIGHT_TOP, g_tint, 0.05f), 255),
              rgb_pack(rgb_mix(NIGHT_BOTTOM, g_tint, 0.18f), 255));
    lattice_draw(t, g_tint);
    unsigned t1 = now_us();
    draw_chrome(catalog);
    if (catalog->count > 0 && g_view_count > 0) {
        draw_list(catalog, cursor, t);
        draw_panel(&catalog->apps[g_view[cursor < g_view_count ? cursor : 0]], t);
    } else if (g_status[0]) {
        /* Nothing to browse yet: the word stands in the room, leaning
           slowly, lit from behind, and under it what is being waited for.
           The dots count the seconds. */
        float cx = SCR_W / 2.0f, cy = 116.0f;
        title_draw(cx, cy, t, g_tint);
        float w = font_width(FONT_META, g_status);
        font_print(FONT_META, cx - w / 2, cy + 74, g_dim, g_status);
    } else {
        font_print(FONT_BODY, LIST_X, 120, g_dim, "The catalog came back empty.");
    }
    if (g_installing) draw_install();
    draw_footer();
    if (g_fade > 0) {
        gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, g_fade));
        g_fade -= 7;
    }
    unsigned t2 = now_us();
    gfx_frame_end();
    unsigned t3 = now_us();
    if (t3 - t0 > g_worst_total) {
        g_worst_total = t3 - t0;
        g_worst_back = t1 - t0;
        g_worst_front = t2 - t1;
        g_worst_end = t3 - t2;
    }
}

int shell_settled(void) {
    float target = LIST_Y + (g_cursor - g_first) * ITEM_H;
    float bar = g_sel_y - target;
    int picture_done = !g_catalog || g_catalog->count <= 0 || preview_settled();
    return g_fade <= 0 && picture_done && bar > -1.0f && bar < 1.0f;
}

/* ------------------------------------------------------------- screenshot */

void shell_shot_sync(const struct catalog *catalog, int cursor) {
    int index = shell_view_index(cursor);
    if (catalog->count <= 0 || index < 0) return;
    const struct app_entry *entry = &catalog->apps[index];

    if (cursor != g_last_cursor) {
        /* The first selection is the app starting up, not the user
           scrolling past: nothing to wait for. */
        int first = g_last_cursor < 0;
        g_last_cursor = cursor;
        g_status[0] = '\0';
        lattice_touch((LIST_X + LIST_W / 2) / (float)SCR_W);
        preview_show(entry, first);
    }
    if (preview_tick()) {
        shell_draw(catalog, cursor);        /* say so before we block */
        preview_load();
    }
}

void shell_word(const char *word) {
    snprintf(g_word, sizeof(g_word), "%s", word ? word : "Connecting");
}

void shell_status(const char *text) {
    snprintf(g_status, sizeof(g_status), "%s", text ? text : "");
}

/* ---------------------------------------------------------------- install */

void shell_install_begin(const char *name) {
    g_installing = 1;
    g_status[0] = '\0';
    snprintf(g_install_name, sizeof(g_install_name), "%s", name ? name : "");
    g_install_phase[0] = '\0';
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
}

void shell_install_phase(void *ctx, const char *phase) {
    (void)ctx;
    snprintf(g_install_phase, sizeof(g_install_phase), "%s", phase ? phase : "");
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_progress(void *ctx, size_t done, size_t total) {
    (void)ctx;
    g_install_done = done;
    g_install_total = total;
    /* Every frame costs a vblank wait, which would throttle the download
       itself; four a second is enough to look alive. */
    if (done != total && !expired(g_install_drawn_ms, 250)) return;
    g_install_drawn_ms = now_ms();
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_end(const char *message) {
    g_installing = 0;
    snprintf(g_status, sizeof(g_status), "%s", message ? message : "");
    logline("%s", g_status);
    lattice_touch(0.5f);
}
