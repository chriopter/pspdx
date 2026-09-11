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

#include <pspkernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "logic/entropy.h"
#include "network/https.h"
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
static char g_install_name[56];         /* a name, and room for "2 of 3: " */
static char g_install_phase[16];
static size_t g_install_done, g_install_total;
static float g_bar;                     /* the drawn end, chasing the reported one */
static unsigned g_install_drawn_ms;
static char g_status[96];
/* What stands over the browser, if anything: a question, a short menu with a
   cursor on one of its choices, or the info band. All of it is set from
   outside and only drawn here -- what is pressed in answer, and which row the
   cursor is on, is the main loop's business. */
static char g_ask_title[64], g_ask_line[96];
#define MENU_MAX 4
static char g_menu_title[48], g_menu_item[MENU_MAX][32];
static unsigned char g_menu_on[MENU_MAX];
static int g_menu_count, g_menu_cursor;
#define INFO_ACTIONS 2
static int g_info, g_info_action;
/* The share of a frame that goes into drawing it, eased over about a second
   so the number on screen does not flicker. */
static float g_load;
static float g_frame_us;                /* frame to frame, eased, for the fps */
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

/* Two tabs are not categories and are named by a sign rather than a word: the
   updates waiting for the packages on the stick, and the basket this session
   has filled. They are numbered below zero so that a tab is either an index
   into TAB_NAME or one of these, with nothing to keep in step, and they stand
   to the left of All because what is waiting to be done comes before what is
   merely there to browse. */
#define TAB_UPDATES (-2)
#define TAB_BASKET  (-1)

static int g_tab[TAB_ALL + 2];          /* which of them have anything */
static int g_tabs;
static int g_tab_at;                    /* index into g_tab, not into TAB_NAME */
static const struct catalog *g_view_of;
static unsigned char g_view[MAX_APPS];
static int g_view_count;                /* packages; the action row is extra */
static int g_view_action;               /* 1 when row 0 is the action row */

/* The basket: catalog indices set aside this session, a bit each. */
static unsigned char g_basket[(MAX_APPS + 7) / 8];
static int g_basket_n;

int shell_basket_has(int index) {
    if (index < 0 || index >= MAX_APPS) return 0;
    return (g_basket[index >> 3] >> (index & 7)) & 1;
}

int shell_basket_count(void) { return g_basket_n; }

void shell_basket_toggle(int index) {
    if (index < 0 || index >= MAX_APPS) return;
    g_basket[index >> 3] ^= (unsigned char)(1u << (index & 7));
    g_basket_n += shell_basket_has(index) ? 1 : -1;
}

void shell_basket_forget(int index) {
    if (shell_basket_has(index)) shell_basket_toggle(index);
}

void shell_basket_clear(void) {
    memset(g_basket, 0, sizeof(g_basket));
    g_basket_n = 0;
}

/* How many packages on the stick have a newer one published. The number is
   the updates tab's own label and the reason it exists at all, so it is asked
   for rather than remembered. */
static int updates_waiting(void) {
    int n = 0;
    if (!g_view_of) return 0;
    for (int i = 0; i < g_view_of->count; i++)
        if (g_view_of->apps[i].state == APP_UPDATE) n++;
    return n;
}

/* restart is for a view whose rows now stand for other packages than they
   did: the list goes back to the top and the card is told to fetch afresh.
   A view merely rebuilt under the same tab keeps where it was scrolled to. */
static void build_view(int restart) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    g_view_count = 0;
    g_view_action = 0;
    if (!g_view_of || g_tabs <= 0) return;
    for (int i = 0; i < g_view_of->count; i++) {
        int take;
        if (tab == TAB_UPDATES) take = g_view_of->apps[i].state == APP_UPDATE;
        else if (tab == TAB_BASKET) take = shell_basket_has(i);
        else take = !TAB_KEY[tab][0] ||
                    strcmp(g_view_of->apps[i].category, TAB_KEY[tab]) == 0;
        if (take) g_view[g_view_count++] = (unsigned char)i;
    }
    /* A tab that is a job rather than a category carries the job itself at
       the top, above the packages it would be done to. */
    g_view_action = tab < 0;
    if (!restart) return;
    g_first = 0;
    g_last_cursor = -1;
}

/* Which tabs have anything in them, in the order they are shown, and where
   the one named by keep ended up. Returns 0 if keep did not survive. */
static int collect_tabs(int keep) {
    int found = 0;
    g_tabs = 0;
    g_tab_at = 0;
    if (!g_view_of || g_view_of->count <= 0) return 0;
    if (updates_waiting() > 0) g_tab[g_tabs++] = TAB_UPDATES;
    if (g_basket_n > 0) g_tab[g_tabs++] = TAB_BASKET;
    for (int t = 0; t < TAB_ALL; t++) {
        int has = !TAB_KEY[t][0];
        for (int i = 0; !has && i < g_view_of->count; i++)
            has = strcmp(g_view_of->apps[i].category, TAB_KEY[t]) == 0;
        if (has) g_tab[g_tabs++] = t;
    }
    for (int i = 0; i < g_tabs; i++)
        if (g_tab[i] == keep) { g_tab_at = i; found = 1; }
    return found;
}

void shell_view_rebuild(const struct catalog *catalog) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    /* A fetch rewrites the array the basket's indices point into, and row
       seventeen of the new catalog is not the package row seventeen of the
       old one was. Nothing is carried across. */
    shell_basket_clear();
    g_view_of = catalog;
    collect_tabs(was);
    build_view(1);
}

int shell_tabs_refresh(void) {
    int was = g_tabs ? g_tab[g_tab_at] : 0;
    int kept = collect_tabs(was);
    build_view(!kept);
    return kept;
}

int shell_view_count(void) { return g_view_count + g_view_action; }

int shell_view_action(int row) { return g_view_action && row == 0; }

int shell_view_index(int row) {
    if (shell_view_action(row)) return SHELL_ROW_ACTION;
    row -= g_view_action;
    return row >= 0 && row < g_view_count ? g_view[row] : -1;
}

int shell_view_row(int index) {
    for (int row = 0; row < g_view_count; row++)
        if (g_view[row] == index) return row + g_view_action;
    return -1;
}

enum shell_tab_kind shell_tab_kind(void) {
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    return tab == TAB_UPDATES ? SHELL_TAB_UPDATES
         : tab == TAB_BASKET ? SHELL_TAB_BASKET : SHELL_TAB_CATEGORY;
}

void shell_action_plan(struct shell_plan *plan) {
    memset(plan, 0, sizeof(*plan));
    if (!g_view_of || !g_view_action) return;
    plan->updates = g_tab[g_tab_at] == TAB_UPDATES;
    for (int row = 0; row < g_view_count; row++) {
        const struct app_entry *entry = &g_view_of->apps[g_view[row]];
        /* Without a release in the catalog the size is only known once a
           manifest has been fetched, and a run of installs that cannot say
           beforehand what it will download is not one to offer in a single
           press. Those are counted and left out. */
        if (!entry->has_release || !entry->release.size) { plan->skipped++; continue; }
        plan->apps++;
        plan->bytes += entry->release.size;
        if (entry->state == APP_CURRENT) plan->again++;
    }
}

int shell_tab_count(void) { return g_tabs; }

void shell_tab_move(int step) {
    if (g_tabs <= 1) return;
    g_tab_at = (g_tab_at + step + g_tabs) % g_tabs;
    build_view(1);
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

/* ------------------------------------------------------------------ marks */

/* The signs that are not letters. A ribbon is a band offset up and down from
   its own points: it draws a slope well and a small closed shape not at all,
   and the turning arrows drawn that way came out as a cluster of faint dots
   on the PSP's real 480 by 272. So these are written out pixel by pixel
   instead -- twelve rows of twelve characters, '#' where a pixel goes -- and
   what is on screen is what is in the source. Twelve because a circle of two
   arrows needs a two-pixel stroke, a head wider than that stroke, and a pixel
   of daylight between a head and the tail it is chasing, and eleven does not
   have room for all three. */
#define GLYPH_W 12
#define GLYPH_H 12

/* The system's own sign for an update: two chunky arrows chasing each other
   head to tail round a circle. Each is a two-pixel arc of most of a half
   turn, ending in a solid head that flares to five pixels across its base and
   comes back to two at the tip -- at this size a head one pixel wider than
   its stroke is a bump and not an arrowhead. The gap before the other arrow's
   tail is what keeps it from reading as a ring with two notches in it. The
   figure is the same turned half round, which is what says the two arrows are
   the same arrow twice rather than one broken one. */
static const char *const GLYPH_UPDATE[GLYPH_H] = {
    "............",
    "...######...",
    "..########..",
    ".###....###.",
    ".##....#####",
    ".........##.",
    ".##.........",
    "#####....##.",
    ".###....###.",
    "..########..",
    "...######...",
    "............",
};

/* A basket: a handle, a rim the full width of the box, and a body that tapers
   to its foot. Solid rather than woven -- at twelve pixels a weave is noise,
   and what has to read is the silhouette. */
static const char *const GLYPH_BASKET[GLYPH_H] = {
    "............",
    "....####....",
    "...##..##...",
    "...##..##...",
    "############",
    "############",
    ".##########.",
    "..########..",
    "..########..",
    "...######...",
    "...######...",
    "............",
};

static void draw_bitmap(const char *const *rows, float left, float top,
                        unsigned color) {
    int x = (int)(left + 0.5f), y = (int)(top + 0.5f);
    for (int r = 0; r < GLYPH_H; r++)
        for (int c = 0; rows[r][c]; c++)
            if (rows[r][c] == '#') gfx_rect(x + c, y + r, 1, 1, color);
}

/* What the update sign is lit by: no spin -- a shape this small turning is a
   shape flickering -- but a slow swell of brightness, so a tab or a row with
   an update on it is alive without ever moving. */
static float update_pulse(float t) { return 0.85f + 0.15f * sinf(t * 1.6f); }

/* Every caller has a centre rather than a corner. */
static void draw_glyph(const char *const *rows, float cx, float cy,
                       unsigned color) {
    draw_bitmap(rows, cx - GLYPH_W / 2.0f, cy - GLYPH_H / 2.0f, color);
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

/* The same colour, quieter: a mark on a row the eye is not on should be
   read only when it is looked for. */
static unsigned faded(unsigned color, int alpha) {
    return (color & 0x00FFFFFFu) | ((unsigned)alpha << 24);
}

/* The green-white an update is said in, wherever it is said. */
#define UPDATE_RGB RGB(170, 255, 190)

/* ---------------------------------------------------------------- sizes */

/* Megabytes to a tenth: whole megabytes call everything under one of them
   nothing, and a count of bytes is not a size anybody reads. */
static void size_mb(unsigned long long bytes, char *out, size_t size) {
    snprintf(out, size, "%lu.%lu MB", (unsigned long)(bytes >> 20),
             (unsigned long)((bytes * 10 >> 20) % 10));
}

/* What the wait will be. A PSP-1004's 802.11b radio and TCP stack were
   measured at 180 KB/s, which is the only honest number to quote here --
   the emulator's host link would promise a minute the hardware cannot keep. */
#define PSP_KB_PER_S 180

static void download_time(unsigned long long bytes, char *out, size_t size) {
    unsigned secs = (unsigned)(bytes / (PSP_KB_PER_S * 1024));
    if (secs < 90) snprintf(out, size, "%u s", secs);
    else snprintf(out, size, "%u min", (secs + 30) / 60);
}

/* ------------------------------------------------------------------ chrome */

/* Under a block of text the floor is dimmed, but with a soft spot and not
   a plate: the lattice runs everywhere, it only gets quieter here. */
static void draw_shade(int cx, int cy, int w, int h) {
    gfx_shade(cx, cy, w * 1.6f, h * 1.8f, 170);
}

/* A tab that is a job rather than a category is a sign and a number -- the
   turning arrows and how many wait, the basket and what is in it -- because
   the sign is the same one the rows below it carry and a word would not be.
   The number is built into a static, so it is read before the next call. */
static const char *tab_count(int tab) {
    static char text[8];
    snprintf(text, sizeof(text), "%d",
             tab == TAB_UPDATES ? updates_waiting() : g_basket_n);
    return text;
}

static float tab_width(int tab) {
    if (tab >= 0) return font_width(FONT_META, TAB_NAME[tab]);
    return GLYPH_W + 4 + font_width(FONT_META, tab_count(tab));
}

/* The tabs sit between the name and the count, spread across whatever room
   the two of them leave. The active one is lit rather than boxed: a word in
   the text colour with the room's own light welling up under it. */
static void draw_tabs(float left, float right, float t) {
    if (g_tabs <= 1) return;
    float words = 0;
    for (int i = 0; i < g_tabs; i++) words += tab_width(g_tab[i]);
    float gap = (right - left - words) / (g_tabs - 1);
    if (gap > 22) gap = 22;
    if (gap < 7) gap = 7;
    float x = left + (right - left - words - gap * (g_tabs - 1)) / 2;
    if (x < left) x = left;
    for (int i = 0; i < g_tabs; i++) {
        int tab = g_tab[i], on = i == g_tab_at;
        float w = tab_width(tab);
        if (on) {
            gfx_glow(x + w / 2, 17, w + 30, 30, rgb_pack(g_tint, 110));
            gfx_glow(x + w / 2, 24, w + 8, 7,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.6f), 120));
        }
        if (tab >= 0) {
            font_print(FONT_META, x, 21, on ? g_text : g_dim, TAB_NAME[tab]);
        } else {
            int lit = on ? 255 : 150;
            if (tab == TAB_UPDATES) {
                float pulse = update_pulse(t);
                gfx_glow(x + GLYPH_W / 2.0f, 16, 30, 30,
                         RGBA(140, 255, 170, (int)((on ? 120 : 60) * pulse)));
                draw_bitmap(GLYPH_UPDATE, x, 12,
                            faded(UPDATE_RGB, (int)(lit * pulse)));
            } else {
                draw_bitmap(GLYPH_BASKET, x, 12, faded(on ? g_text : g_dim, lit));
            }
            font_print(FONT_META, x + GLYPH_W + 4, 21, on ? g_text : g_dim,
                       tab_count(tab));
        }
        x += w + gap;
    }
}

static void draw_chrome(const struct catalog *catalog, float t) {
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
       the tab's own word under a category, and under the two tabs that are a
       job rather than a category, the job -- so the number beside the sign in
       the tab is said again in words at the other end of the header. It
       changes when the catalog, the basket or the tab does, which is not
       every frame. */
    static char right[64];
    static int said_total = -1, said_updates = -1, said_tab = -1, said_basket = -1;
    int tab = g_tabs ? g_tab[g_tab_at] : 0;
    int shown = tab != 0 ? g_view_count : catalog->total;
    int updates = 0;
    for (int row = 0; row < g_view_count; row++)
        if (catalog->apps[g_view[row]].state == APP_UPDATE) updates++;
    if (shown != said_total || updates != said_updates || tab != said_tab ||
        g_basket_n != said_basket) {
        said_total = shown;
        said_updates = updates;
        said_tab = tab;
        said_basket = g_basket_n;
        if (tab == TAB_UPDATES)
            snprintf(right, sizeof(right), "%d update%s", shown,
                     shown == 1 ? "" : "s");
        else if (tab == TAB_BASKET)
            snprintf(right, sizeof(right), "%d in the basket", shown);
        else {
            const char *what = tab ? TAB_KEY[tab] : "apps";
            if (updates)
                snprintf(right, sizeof(right), "%d %s  %d update%s", shown, what,
                         updates, updates == 1 ? "" : "s");
            else
                snprintf(right, sizeof(right), "%d %s", shown, what);
        }
    }
    if (catalog->count > 0) {
        float x = SCR_W - LIST_X - font_width(FONT_META, right);
        font_print(FONT_META, x, 21, g_dim, right);
        draw_tabs(LIST_X + font_width(FONT_H1, "PSPDX") + 22, x - 16, t);
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

/* The two words the action row is headed with, and the sentence under them.
   Both are wanted in the list and again in the panel, so they are made in one
   place. */
static const char *action_title(void) {
    return shell_tab_kind() == SHELL_TAB_UPDATES ? "Update all" : "Download all";
}

/* "3 apps, 61.5 MB" -- or, when nothing in the tab can be fetched without a
   manifest first, what is in the way instead. */
static const char *action_line(void) {
    static char line[48];
    struct shell_plan plan;
    char size[24];
    shell_action_plan(&plan);
    if (plan.apps <= 0)
        snprintf(line, sizeof(line), "nothing here has a release");
    else {
        size_mb(plan.bytes, size, sizeof(size));
        snprintf(line, sizeof(line), "%d app%s, %s", plan.apps,
                 plan.apps == 1 ? "" : "s", size);
    }
    return line;
}

/* The row the tab itself sits on. Two lines rather than one: at this width
   the heading and the tally do not fit on a line together in the list's own
   face, and stacked they read as a heading with its tally under it, which is
   what they are. Where a package would have its icon, the tab's own sign. */
static void draw_action_row(int y, int selected, float t) {
    int updates = shell_tab_kind() == SHELL_TAB_UPDATES;
    float gx = LIST_X + ICON_W / 2.0f, gy = y + ITEM_H / 2.0f;
    if (updates) {
        float pulse = update_pulse(t);
        gfx_glow(gx, gy, 36, 36, RGBA(140, 255, 170, (int)((selected ? 150 : 70) * pulse)));
        draw_glyph(GLYPH_UPDATE, gx, gy,
                   faded(UPDATE_RGB, (int)((selected ? 255 : 153) * pulse)));
    } else {
        draw_glyph(GLYPH_BASKET, gx, gy, selected ? g_text : faded(g_dim, 170));
    }
    int w = LIST_X + LIST_W - NAME_X;
    font_print_clipped(FONT_BODY, NAME_X, y + 13, w, selected ? g_text : g_dim,
                       action_title());
    font_print_clipped(FONT_META, NAME_X, y + 25, w, g_dim, action_line());
}

static void draw_list(const struct catalog *catalog, int cursor, float t) {
    int count = shell_view_count();
    if (cursor < g_first) g_first = cursor;
    if (cursor >= g_first + VISIBLE) g_first = cursor - VISIBLE + 1;
    if (g_first < 0) g_first = 0;

    /* The bar chases the selection rather than jumping to it. A quarter of
       the remaining distance per frame settles in about a fifth of a second
       and never overshoots. */
    float target = LIST_Y + (cursor - g_first) * ITEM_H;
    g_sel_y += (target - g_sel_y) * 0.25f;

    int rows = count < VISIBLE ? count : VISIBLE;
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
       handed over as the entries they stand for -- and the action row stands
       for none, so it asks for nothing. */
    int wanted[VISIBLE], want_count = 0;
    for (int i = g_first; i < count && i < g_first + VISIBLE; i++) {
        int index = shell_view_index(i);
        if (index >= 0) wanted[want_count++] = index;
    }
    icons_bind(catalog);
    if (icons_want(wanted, want_count)) preview_poke();

    for (int i = g_first; i < count && i < g_first + VISIBLE; i++) {
        int y = LIST_Y + (i - g_first) * ITEM_H;
        int selected = i == cursor;
        int index = shell_view_index(i);
        if (index == SHELL_ROW_ACTION) { draw_action_row(y, selected, t); continue; }
        if (index < 0) continue;
        const struct app_entry *entry = &catalog->apps[index];

        /* The bundle's own icon, dimmed with the name; a dark plate where
           it has not arrived, so the column reads as a column. */
        const struct gfx_texture *icon = icons_get(index);
        int iy = y + (ITEM_H - ICON_H) / 2;
        if (icon)
            gfx_texture_draw(icon, LIST_X, iy, ICON_W, ICON_H,
                             selected ? RGB(255, 255, 255) : RGB(150, 150, 150));
        else
            gfx_rect(LIST_X, iy, ICON_W, ICON_H, RGBA(255, 255, 255, selected ? 24 : 12));

        /* Marks, not words, at the end of the row, read from the outside in:
           where the package stands -- the line ticked off when it is on the
           stick, the system's turning arrows when a newer one waits, nothing
           for the rest -- and then, inside that, the basket if this session
           has set the package aside. */
        float mx = LIST_X + LIST_W - 8, my = y + ITEM_H / 2 - 1;
        int name_w = LIST_X + LIST_W - NAME_X;
        if (entry->state == APP_UPDATE) {
            float pulse = update_pulse(t);
            gfx_glow(mx, my, 34, 34,
                     RGBA(140, 255, 170, (int)((selected ? 150 : 70) * pulse)));
            draw_glyph(GLYPH_UPDATE, mx, my,
                       faded(UPDATE_RGB, (int)((selected ? 255 : 153) * pulse)));
            mx -= 17;
            name_w -= 19;
        } else if (entry->state != APP_NOT_INSTALLED) {
            draw_tick(mx, my, selected ? g_accent : faded(g_dim, 153));
            mx -= 17;
            name_w -= 19;
        }
        if (shell_basket_has(index)) {
            draw_glyph(GLYPH_BASKET, mx, my, faded(g_dim, selected ? 190 : 120));
            name_w -= 17;
        }

        font_print_clipped(FONT_BODY, NAME_X, y + 21, name_w,
                           selected ? g_text : g_dim, entry->name);
    }

    if (count > VISIBLE) {
        int track = FOOTER_Y - 6 - LIST_Y;
        int knob = track * VISIBLE / count;
        int at = track * g_first / count;
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y, 2, track, RGBA(255, 255, 255, 24));
        gfx_rect(LIST_X + LIST_W + 10, LIST_Y + at, 2, knob, g_accent);
    }
}

/* ------------------------------------------------------------------ panel */

/* The card's half of the screen when the cursor is on the action row. There
   is no picture: a row that stands for several packages has no one screenshot
   to show, and a card left holding the last package's would be a lie about
   what X is going to fetch. What the space is worth instead is the bill --
   every package that would come down, what each weighs, the total, and how
   long that is over a PSP's own radio. */
static void draw_action_panel(const struct catalog *catalog, float t) {
    struct shell_plan plan;
    char value[48], size[24];
    int y = SHOT_Y + 14;

    shell_action_plan(&plan);
    draw_shade(PANEL_X + SHOT_W / 2, y + 70, SHOT_W, 170);
    gfx_glow(PANEL_X + SHOT_W / 2, y + 60, SHOT_W + 90, 200,
             rgb_pack(g_tint, (int)(70 * update_pulse(t))));

    font_print(FONT_H1, PANEL_X, y, g_text, action_title());
    if (plan.apps > 0) {
        size_mb(plan.bytes, size, sizeof(size));
        download_time(plan.bytes, value, sizeof(value));
        font_printf(FONT_META, PANEL_X, y + 20, g_dim, "%s to download, about %s",
                    size, value);
    } else {
        font_print(FONT_META, PANEL_X, y + 20, g_dim,
                   "nothing here can be fetched without a manifest first");
    }
    /* One line a package, in the order they would be fetched, for as many as
       the panel holds; the rest are counted rather than named. */
    int line = 0, room = (FOOTER_Y - 20 - (y + 40)) / 14;
    for (int row = 0; row < shell_view_count(); row++) {
        int index = shell_view_index(row);
        if (index < 0) continue;
        const struct app_entry *entry = &catalog->apps[index];
        if (!entry->has_release || !entry->release.size) continue;
        if (line >= room) {
            font_printf(FONT_META, PANEL_X, y + 40 + line * 14, g_dim,
                        "and %d more", plan.apps - line);
            line++;
            break;
        }
        size_mb(entry->release.size, size, sizeof(size));
        float sw = font_width(FONT_META, size);
        font_print_clipped(FONT_META, PANEL_X, y + 40 + line * 14,
                           SHOT_W - sw - 10,
                           entry->state == APP_UPDATE ? UPDATE_RGB : g_text,
                           entry->name);
        font_print(FONT_META, PANEL_X + SHOT_W - sw, y + 40 + line * 14,
                   g_dim, size);
        line++;
    }
    if (plan.skipped)
        font_printf(FONT_META, PANEL_X, y + 46 + line * 14, g_dim,
                    "%d without a release, left out", plan.skipped);
}

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
    /* No sweep across the glass: a highlight travelling over a picture reads
       as a smear on the screen rather than as light in the room. The light
       that used to cross the card crosses the water now, where it has a
       surface to lie on. */
    card.gloss = -1.0f;

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

/* Everything modal is the same shape, the one the system's own message dialog
   has: a band the full width of the screen, the room still moving above and
   below it, held by a line of light along each edge that fades away toward
   both walls. Nothing here has a left side, a right side or a corner. */
#define BAND_H 118
#define BAND_Y ((SCR_H - BAND_H) / 2)
#define MENU_H 156
#define MENU_Y ((SCR_H - MENU_H) / 2)
#define INFO_H 190
#define INFO_Y ((SCR_H - INFO_H) / 2)

static void band_edge(int y) {
    unsigned bright = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.55f), 210);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(0, y, SCR_W / 2, 1, clear, bright);
    gfx_hgrad(SCR_W / 2, y, SCR_W / 2, 1, bright, clear);
    /* The line is one pixel; what makes it read as light is under it. */
    gfx_glow(SCR_W / 2, y, 460, 9, rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), 70));
}

/* A line of the same kind but shorter, for dividing a band's inside. */
static void band_rule(int y, int half_w, int alpha) {
    unsigned faint = rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.4f), alpha);
    unsigned clear = rgb_pack(g_tint, 0);
    gfx_hgrad(SCR_W / 2 - half_w, y, half_w, 1, clear, faint);
    gfx_hgrad(SCR_W / 2, y, half_w, 1, faint, clear);
}

static void draw_band(int y, int h) {
    /* The whole room steps back a little so the band is the front. */
    gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, 110));
    gfx_rect(0, y, SCR_W, h, RGBA(0, 0, 0, 215));
    /* Flat dark over the screenshot card still leaves it the brightest thing
       on screen and the text on top of it unreadable, so the band is darkest
       where the words are and lets the room back in toward the walls. */
    gfx_shade(SCR_W / 2.0f, y + h / 2.0f, SCR_W * 1.7f, h * 1.1f, 170);
    band_edge(y);
    band_edge(y + h);
}

/* Straight lines a pixel wide, stepped along whichever axis is the longer:
   what the room's other strokes are drawn with offsets only in y, which turns
   a diagonal into a parallelogram and a ring into a spiral. The face buttons
   are nine pixels across and the honest way at that size is to place the
   pixels. */
static void stroke(float x0, float y0, float x1, float y1, unsigned color) {
    float dx = x1 - x0, dy = y1 - y0;
    float span = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
               ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    int steps = (int)(span + 0.5f);
    for (int i = 0; i <= steps; i++) {
        float f = steps ? (float)i / steps : 0.0f;
        gfx_rect((int)(x0 + dx * f + 0.5f), (int)(y0 + dy * f + 0.5f), 1, 1, color);
    }
}

/* The face buttons, drawn rather than written: the system font has no glyph
   for them, and spelling TRIANGLE out is longer than the word it would be
   labelling. A ribbon is a band offset up and down from its own points, so it
   draws a slope well and a vertical line not at all -- which is why these are
   placed pixel by pixel instead, all four in the same nine-pixel box with the
   same one-pixel line, so no one of them reads as bolder than the rest. */
enum mark { MARK_CROSS, MARK_CIRCLE, MARK_SQUARE, MARK_TRIANGLE };

#define MARK_W 11
#define MARK_R 4.0f

static void draw_mark(enum mark mark, float cx, float cy, unsigned color) {
    const float r = MARK_R;
    if (mark == MARK_CROSS) {
        stroke(cx - r + 1, cy - r + 1, cx + r - 1, cy + r - 1, color);
        stroke(cx - r + 1, cy + r - 1, cx + r - 1, cy - r + 1, color);
    } else if (mark == MARK_TRIANGLE) {
        stroke(cx, cy - r, cx + r, cy + r - 1, color);
        stroke(cx + r, cy + r - 1, cx - r, cy + r - 1, color);
        stroke(cx - r, cy + r - 1, cx, cy - r, color);
    } else if (mark == MARK_SQUARE) {
        int x = (int)(cx - r + 1), y = (int)(cy - r + 1), n = (int)(2 * r - 1);
        gfx_rect(x, y, n, 1, color);
        gfx_rect(x, y + n - 1, n, 1, color);
        gfx_rect(x, y, 1, n, color);
        gfx_rect(x + n - 1, y, 1, n, color);
    } else {
        /* Round, and round the whole way: a point per pixel of circumference,
           which at this radius is a closed ring of even weight. */
        for (int i = 0; i < 26; i++) {
            float a = i * (6.2831853f / 26);
            gfx_rect((int)(cx + cosf(a) * r + 0.5f),
                     (int)(cy + sinf(a) * r + 0.5f), 1, 1, color);
        }
    }
}

/* One button and the word for what it does, the mark sitting on the text's
   own line. Returns where the next one may start; a row that has to be
   centred is measured with the same arithmetic first. */
static float hint_width(const char *text) {
    return MARK_W + 4 + font_width(FONT_META, text);
}

static float draw_hint(float x, float base, enum mark mark, const char *text,
                       unsigned color) {
    draw_mark(mark, x + MARK_W / 2.0f, base - 4, color);
    return font_print(FONT_META, x + MARK_W + 4, base, color, text) + 16;
}

/* The row of buttons a band carries at its foot, centred and both the same
   weight: which one is taken is decided by the button pressed, not by a
   cursor sitting on one of them. */
static void draw_answers(float base, const char *yes, const char *no) {
    float x = SCR_W / 2 - (hint_width(yes) + 30 + hint_width(no)) / 2;
    draw_mark(MARK_CROSS, x + MARK_W / 2.0f, base - 4, g_dim);
    x = font_print(FONT_META, x + MARK_W + 4, base, g_text, yes) + 30;
    draw_mark(MARK_CIRCLE, x + MARK_W / 2.0f, base - 4, g_dim);
    font_print(FONT_META, x + MARK_W + 4, base, g_text, no);
}

/* ------------------------------------------------------------------- ask */

static void draw_ask(void) {
    draw_band(BAND_Y, BAND_H);
    float w = font_width(FONT_BODY, g_ask_title);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, BAND_Y + 44, SCR_W - 40,
                       g_text, g_ask_title);
    w = font_width(FONT_META, g_ask_line);
    font_print_clipped(FONT_META, SCR_W / 2 - w / 2, BAND_Y + 68, SCR_W - 40,
                       g_dim, g_ask_line);
    draw_answers(BAND_Y + BAND_H - 22, "Yes", "No");
}

/* ------------------------------------------------------------------ menu */

static void draw_menu(void) {
    draw_band(MENU_Y, MENU_H);

    float w = font_width(FONT_BODY, g_menu_title);
    font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, MENU_Y + 32, SCR_W - 40,
                       g_text, g_menu_title);
    band_rule(MENU_Y + 42, 150, 110);

    /* Grey is the whole of what says a choice cannot be taken: no line
       through it, no bracket after it. */
    unsigned grey = rgb_pack(rgb_mix(NIGHT_BOTTOM, RGB_WHITE, 0.32f), 255);
    for (int i = 0; i < g_menu_count; i++) {
        int y = MENU_Y + 72 + i * 22;
        int on = i == g_menu_cursor;
        if (on) {
            gfx_glow(SCR_W / 2, y - 5, 380, 34, rgb_pack(g_tint, 120));
            gfx_glow(SCR_W / 2, y + 5, 320, 9,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 150));
        }
        unsigned color = !g_menu_on[i] ? grey : on ? g_text : g_dim;
        w = font_width(FONT_BODY, g_menu_item[i]);
        font_print_clipped(FONT_BODY, SCR_W / 2 - w / 2, y, SCR_W - 40, color,
                           g_menu_item[i]);
    }
    draw_answers(MENU_Y + MENU_H - 24, "Select", "Back");
}

/* --------------------------------------------------------------- install */

static void draw_install(void) {
    draw_band(BAND_Y, BAND_H);

    int bar_x = 60, bar_w = SCR_W - 120, bar_y = BAND_Y + 84;
    font_print_clipped(FONT_BODY, bar_x, BAND_Y + 44, bar_w, g_text, g_install_name);
    font_print(FONT_META, bar_x, BAND_Y + 68, g_dim, g_install_phase);

    /* Three pixels of line, not a trough with a fill: the bar is the same
       kind of thing as the band's own edges. */
    gfx_rect(bar_x, bar_y, bar_w, 3, RGBA(255, 255, 255, 28));
    if (g_install_total) {
        /* Progress arrives in whatever lumps the network hands over, and a
           bar that steps by those lumps reads as a stall between them. The
           drawn end chases the reported one instead, a third of the way per
           frame, so it is always moving and never ahead. */
        float target = (float)g_install_done / g_install_total;
        g_bar += (target - g_bar) * 0.34f;
        if (target >= 1.0f && g_bar > 0.995f) g_bar = 1.0f;
        int filled = (int)(bar_w * g_bar + 0.5f);
        gfx_hgrad(bar_x, bar_y, filled, 3, rgb_pack(g_tint, 255), g_accent);
        gfx_glow(bar_x + filled, bar_y + 1, 44, 22, rgb_pack(RGB_WHITE, 150));
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%",
                 (int)((unsigned long long)g_install_done * 100 / g_install_total));
        font_print(FONT_META, bar_x + bar_w - font_width(FONT_META, pct),
                   BAND_Y + 68, g_dim, pct);
    } else if (g_install_done) {
        /* No content-length: show that bytes are moving, not how far. */
        int slide = (int)(gfx_frames() * 3 % (unsigned)bar_w);
        int w = 40 > bar_w - slide ? bar_w - slide : 40;
        gfx_hgrad(bar_x + slide, bar_y, w, 3, g_accent, rgb_pack(g_tint, 0));
    }
}

/* -------------------------------------------------------------- info band */

/* Labels end and values begin at the same places all the way down, so the
   rows read as a column of facts and not as a page of sentences. The two
   that need the room -- where the catalog is and what was negotiated with it
   -- have the band to themselves; the short ones pair up. */
#define FACT_LABEL 150
#define FACT_VALUE 166
#define FACT_LABEL2 330
#define FACT_VALUE2 346

static void fact(int y, float label_end, float value_x, float clip,
                 const char *label, const char *value) {
    font_print(FONT_META, label_end - font_width(FONT_META, label), y, g_dim, label);
    font_print_clipped(FONT_META, value_x, y, clip, g_text, value);
}

/* wolfSSL names a suite the way its own tables do -- TLS13-CHACHA20-POLY1305-
   SHA256. In a TLS 1.3 row the version is already said and the hash cannot be
   anything else, so both ends come off and what is left is the part that
   differs between one connection and the next. */
static void tidy_cipher(const char *name, char *out, size_t size) {
    const char *cut = name;
    if (strncmp(cut, "TLS13", 5) == 0) cut += 5;
    else if (strncmp(cut, "TLS", 3) == 0) cut += 3;
    if (*cut == '-' || *cut == '_') cut++;
    snprintf(out, size, "%s", *cut ? cut : name);
    for (char *p = out; *p; p++) if (*p == '_') *p = '-';
    size_t n = strlen(out);
    if (n > 7 && strncmp(out + n - 7, "-SHA", 4) == 0) out[n - 7] = '\0';
}

/* The catalog is named by where it came from, not by which file on it. */
static void url_host(const char *url, char *out, size_t size) {
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    size_t n = strcspn(host, "/");
    if (n >= size) n = size - 1;
    memcpy(out, host, n);
    out[n] = '\0';
}

/* What is left on the stick. The driver counts in clusters and answers
   through a pointer handed to it in a struct, which is the one call in this
   file that looks like firmware because it is. Asked once when the band
   opens: a FAT32 free count walks the allocation table. */
static char g_storage[24] = "?";
static float g_storage_used = -1.0f;    /* below zero while the stick is unread */

static void size_words(unsigned long long bytes, char *out, size_t size) {
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(out, size, "%lu.%lu GB", (unsigned long)(bytes >> 30),
                 (unsigned long)((bytes * 10 >> 30) % 10));
    else
        snprintf(out, size, "%lu MB", (unsigned long)(bytes >> 20));
}

static void read_storage(void) {
    struct ms_info {
        unsigned max_clusters, free_clusters, max_sectors, sector_size, sector_count;
    } info;
    struct { struct ms_info *at; } command = { &info };
    memset(&info, 0, sizeof(info));
    g_storage_used = -1.0f;
    if (sceIoDevctl("ms0:", 0x02425818, &command, sizeof(command), NULL, 0) < 0) {
        snprintf(g_storage, sizeof(g_storage), "unknown");
        return;
    }
    unsigned long long unit = (unsigned long long)info.sector_count * info.sector_size;
    unsigned long long left = info.free_clusters * unit;
    unsigned long long all = info.max_clusters * unit;
    size_words(left, g_storage, sizeof(g_storage));
    if (all) g_storage_used = 1.0f - (float)((double)left / (double)all);
}

/* The two things the band does rather than says. Which one X takes is the
   cursor's, and the cursor is the main loop's. */
static const char *const INFO_ACTION[INFO_ACTIONS] = {
    "Fetch the catalog again",
    "Discard entropy and sweep again",
};

static void draw_info(void) {
    draw_band(INFO_Y, INFO_H);

    const struct https_info *tls = https_last();
    char value[96];

    url_host(catalog_url(), value, sizeof(value));
    fact(INFO_Y + 34, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         "Catalog", value);

    if (tls->cipher[0]) {
        char cipher[48];
        tidy_cipher(tls->cipher, cipher, sizeof(cipher));
        snprintf(value, sizeof(value), "TLS 1.3  %s  %s", cipher, tls->group);
    } else {
        snprintf(value, sizeof(value), "not connected");
    }
    fact(INFO_Y + 56, FACT_LABEL, FACT_VALUE, SCR_W - FACT_VALUE - 30,
         "Connection", value);

    snprintf(value, sizeof(value), "%u ms", tls->handshake_ms);
    fact(INFO_Y + 82, FACT_LABEL, FACT_VALUE, 140, "Handshake", value);

    snprintf(value, sizeof(value), "%d bits", entropy_bits());
    fact(INFO_Y + 104, FACT_LABEL, FACT_VALUE, 140, "Entropy", value);

    /* Not a scheduler's number -- the PSP has none to ask. The share of each
       frame that goes into drawing it; the rest is the wait for vblank, which
       is the only idle this client has. */
    snprintf(value, sizeof(value), "%d fps, %d%% drawing",
             g_frame_us > 0.0f ? (int)(1000000.0f / g_frame_us + 0.5f) : 0,
             (int)(g_load * 100.0f + 0.5f));
    fact(INFO_Y + 126, FACT_LABEL, FACT_VALUE, 160, "Frames", value);

    int installed = 0;
    if (g_catalog)
        for (int i = 0; i < g_catalog->count; i++)
            if (g_catalog->apps[i].state != APP_NOT_INSTALLED) installed++;
    snprintf(value, sizeof(value), "%d", installed);
    fact(INFO_Y + 82, FACT_LABEL2, FACT_VALUE2, 110, "Installed", value);

    snprintf(value, sizeof(value), "%u KB",
             (unsigned)sceKernelTotalFreeMemSize() / 1024);
    fact(INFO_Y + 104, FACT_LABEL2, FACT_VALUE2, 110, "Memory free", value);

    /* Room on the stick is the one fact here that is a proportion, so it is
       drawn as one: the line fills as the stick does, and what is left of it
       is what a package has to fit into. */
    fact(INFO_Y + 126, FACT_LABEL2, FACT_VALUE2, 110, "Stick free", g_storage);
    if (g_storage_used >= 0.0f) {
        int x = FACT_VALUE2, w = SCR_W - FACT_VALUE2 - 30, y = INFO_Y + 134;
        int used = (int)(w * g_storage_used + 0.5f);
        gfx_rect(x, y, w, 3, RGBA(255, 255, 255, 28));
        if (used > 0) gfx_hgrad(x, y, used, 3, rgb_pack(g_tint, 255), g_accent);
    }

    band_rule(INFO_Y + 142, 160, 120);
    for (int i = 0; i < INFO_ACTIONS; i++) {
        int y = INFO_Y + 164 + i * 22;
        int on = i == g_info_action;
        if (on) {
            gfx_glow(SCR_W / 2, y - 5, 380, 32, rgb_pack(g_tint, 110));
            gfx_glow(SCR_W / 2, y + 5, 320, 9,
                     rgb_pack(rgb_mix(g_tint, RGB_WHITE, 0.7f), 140));
        }
        float w = hint_width(INFO_ACTION[i]);
        float x = SCR_W / 2 - w / 2;
        if (on) draw_mark(MARK_CROSS, x + MARK_W / 2.0f, y - 4, g_accent);
        font_print(FONT_META, x + MARK_W + 4, y, on ? g_accent : g_dim,
                   INFO_ACTION[i]);
    }
}

/* ---------------------------------------------------------------- footer */

static void draw_footer(void) {
    gfx_vgrad(0, FOOTER_Y + 1, SCR_W, SCR_H - FOOTER_Y - 1, RGBA(0, 0, 0, 110),
              RGBA(0, 0, 0, 190));
    /* A band carries its own buttons, so the strip under it stays quiet. */
    if (g_ask_title[0] || g_menu_count || g_installing) return;
    if (g_info) {
        font_print(FONT_META, LIST_X, FOOTER_Y + 15, g_dim,
                   "SELECT or O close");
        return;
    }
    /* What is being waited for -- access point, dns, tls handshake -- goes
       where the keys go, in the strip; the word in the room stands alone. */
    if (g_status[0]) {
        font_print_clipped(FONT_META, LIST_X, FOOTER_Y + 15, SCR_W - 2 * LIST_X,
                           g_accent, g_status);
        return;
    }
    /* The keys, and only the keys that do anything: the triggers are worth
       naming once there is a second tab to reach with them. */
    int index = g_catalog && shell_view_count() > 0 ? shell_view_index(g_cursor) : -1;
    const struct app_entry *entry = index >= 0 ? &g_catalog->apps[index] : 0;
    int installed = entry && entry->state != APP_NOT_INSTALLED;
    float x;
    if (index == SHELL_ROW_ACTION) {
        /* On the action row there is one thing X does and nothing else,
           so nothing else is named. */
        x = draw_hint(LIST_X, FOOTER_Y + 15, MARK_CROSS,
                      shell_tab_kind() == SHELL_TAB_UPDATES ? "update all"
                                                            : "download all", g_dim);
    } else {
        x = draw_hint(LIST_X, FOOTER_Y + 15, MARK_CROSS,
                      installed ? "options" : "install", g_dim);
        if (installed) x = draw_hint(x, FOOTER_Y + 15, MARK_SQUARE, "remove", g_dim);
        if (installed) x = font_print(FONT_META, x, FOOTER_Y + 15, g_dim, "START run") + 14;
        /* Triangle sets a package aside for later, and in the basket it is
           the same key that takes it back out again. */
        if (entry)
            x = draw_hint(x, FOOTER_Y + 15, MARK_TRIANGLE,
                          shell_tab_kind() == SHELL_TAB_BASKET ? "remove"
                                                               : "basket", g_dim);
    }
    font_print(FONT_META, x, FOOTER_Y + 15, g_dim,
               g_tabs > 1 ? "L R category   SELECT info   HOME quit"
                          : "SELECT info   HOME quit");
}

/* ------------------------------------------------------------- water light */

/* A light that crosses the water instead of the picture: one pass every
   twelve seconds, lying on the surface at a fixed depth, so perspective gives
   it its shape -- wide, flat, and the same height above the horizon all the
   way across. It is placed the way everything else that stands on the water
   is placed, by projecting a point of the world, and it fades in and out at
   the two ends rather than sliding off an edge. */
#define LIGHT_Z 2.2f
#define LIGHT_EYE_Y (150.0f / GFX_FOCAL)     /* the lattice's own eye height */

static void draw_water_light(float t) {
    float cycle = fmodf(t, 12.0f) / 12.0f;
    /* Far enough past both walls that the fade, not the edge, ends the pass. */
    float wx = (cycle * 2.0f - 1.0f) * (SCR_W * 0.62f * LIGHT_Z / GFX_FOCAL);
    float sx, sy;
    gfx_water_project(wx, -LIGHT_EYE_Y, LIGHT_Z, &sx, &sy);
    float scale = GFX_FOCAL / LIGHT_Z;
    float fade = sinf(cycle * 3.1415927f);
    fade *= fade;
    struct rgb lit = rgb_mix(g_tint, RGB_WHITE, 0.62f);
    gfx_glow(sx, sy, scale * 0.95f, scale * 0.30f, rgb_pack(lit, (int)(95 * fade)));
    gfx_glow(sx, sy, scale * 0.40f, scale * 0.11f, rgb_pack(RGB_WHITE, (int)(70 * fade)));
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
    draw_water_light(t);
    unsigned t1 = now_us();
    draw_chrome(catalog, t);
    if (catalog->count > 0 && shell_view_count() > 0) {
        int rows = shell_view_count();
        int index = shell_view_index(cursor < rows ? cursor : 0);
        draw_list(catalog, cursor, t);
        if (index == SHELL_ROW_ACTION) draw_action_panel(catalog, t);
        else if (index >= 0) draw_panel(&catalog->apps[index], t);
    } else if (g_status[0]) {
        /* Nothing to browse yet: the word stands in the room, lit from
           behind; what it is waiting for is said in the strip below. */
        title_draw(SCR_W / 2.0f, 116.0f, t, g_tint);
    } else {
        font_print(FONT_BODY, LIST_X, 120, g_dim, "The catalog came back empty.");
    }
    if (g_info) draw_info();
    if (g_menu_count) draw_menu();
    if (g_installing) draw_install();
    if (g_ask_title[0]) draw_ask();
    draw_footer();
    if (g_fade > 0) {
        gfx_rect(0, 0, SCR_W, SCR_H, RGBA(0, 0, 0, g_fade));
        g_fade -= 7;
    }
    unsigned t2 = now_us();
    gfx_frame_end();
    unsigned t3 = now_us();
    /* Drawing against the whole frame, which is this one's start to the
       next one's: what is not drawing is the vblank wait inside frame_end. */
    static unsigned began;
    if (began && t0 - began > 1000 && t0 - began < 200000) {
        g_load += ((float)(t2 - t0) / (t0 - began) - g_load) * 0.05f;
        g_frame_us += ((float)(t0 - began) - g_frame_us) * 0.05f;
    }
    began = t0;
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
    if (catalog->count <= 0 || index == -1) return;
    /* The action row is a row with no package behind it, and the card is
       told to show nothing rather than left holding whatever the cursor
       passed on its way here. */
    const struct app_entry *entry = index >= 0 ? &catalog->apps[index] : 0;

    /* A row number is not enough to say the selection changed: removing an
       entry from the basket slides the rows up under a cursor that has not
       moved, and the row then stands for another package. */
    static const struct app_entry *g_shown;
    if (cursor != g_last_cursor || entry != g_shown) {
        /* The first selection is the app starting up, not the user
           scrolling past: nothing to wait for. */
        int first = g_last_cursor < 0;
        g_last_cursor = cursor;
        g_shown = entry;
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

void shell_ask(const char *title, const char *line) {
    snprintf(g_ask_title, sizeof(g_ask_title), "%s", title ? title : "");
    snprintf(g_ask_line, sizeof(g_ask_line), "%s", line ? line : "");
    /* The last install's result has been overtaken by a new question. */
    if (g_ask_title[0]) g_status[0] = '\0';
}

void shell_menu(const char *title, const char *const *items,
                const unsigned char *takeable, int count, int cursor) {
    if (count > MENU_MAX) count = MENU_MAX;
    if (count < 0) count = 0;
    g_menu_count = count;
    g_menu_cursor = cursor;
    if (!count) return;
    snprintf(g_menu_title, sizeof(g_menu_title), "%s", title ? title : "");
    for (int i = 0; i < count; i++) {
        snprintf(g_menu_item[i], sizeof(g_menu_item[i]), "%s", items[i] ? items[i] : "");
        g_menu_on[i] = takeable ? takeable[i] : 1;
    }
    g_status[0] = '\0';
}

void shell_info(int open, int action) {
    if (open && !g_info) read_storage();
    g_info = open;
    g_info_action = action;
}

/* ---------------------------------------------------------------- install */

void shell_install_begin(const char *name, int at, int of) {
    g_installing = 1;
    g_status[0] = '\0';
    /* One band for both: a lone install is its name, and one of a run says
       where in the run it is first, so the line changes as the run goes and
       the name it changes to is the one being fetched. */
    if (of > 1)
        snprintf(g_install_name, sizeof(g_install_name), "%d of %d: %s", at, of,
                 name ? name : "");
    else
        snprintf(g_install_name, sizeof(g_install_name), "%s", name ? name : "");
    g_install_phase[0] = '\0';
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
    g_bar = 0.0f;
}

void shell_install_phase(void *ctx, const char *phase) {
    (void)ctx;
    snprintf(g_install_phase, sizeof(g_install_phase), "%s", phase ? phase : "");
    g_install_done = g_install_total = 0;
    g_install_drawn_ms = 0;
    g_bar = 0.0f;
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_progress(void *ctx, size_t done, size_t total) {
    (void)ctx;
    g_install_done = done;
    g_install_total = total;
    /* Every frame costs a vblank wait on the thread doing the downloading,
       so this cannot run at sixty; at twelve a second the eased end of the
       bar moves the way the system's own does, and the wait overlaps the
       pacing a real radio imposes anyway. */
    if (done != total && !expired(g_install_drawn_ms, 80)) return;
    g_install_drawn_ms = now_ms();
    if (g_catalog) shell_draw(g_catalog, g_cursor);
}

void shell_install_end(const char *message) {
    g_installing = 0;
    snprintf(g_status, sizeof(g_status), "%s", message ? message : "");
    logline("%s", g_status);
    lattice_touch(0.5f);
}
