/* ===========================================================================
 *  BoltOS  -  kernel/app_music.c
 *  Music: a tiny chiptune player. Ships a few public-domain melodies, synthesises
 *  them to 16-bit/48 kHz square-wave PCM on the fly (integer only, no FPU), and
 *  streams them to the AC'97 codec WITHOUT blocking the desktop: each ~1 s batch
 *  is pushed with audio_play_async, and the window tick() pushes the next batch
 *  once the DMA engine drains (audio_busy() == 0). A bar visualiser animates while
 *  it plays. Needs a real PCM device (QEMU -device AC97); otherwise it says so.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "audio.h"
#include "keyboard.h"
#include "string.h"

#define SR     48000
#define BATCH  48000               /* frames synthesised per push (~1 s)         */

typedef struct { uint16_t freq, ms; } note_t;

/* note frequencies: C4 262 D4 294 E4 330 F4 349 G4 392 A4 440 B4 494 C5 523 */
static const note_t ode[] = {       /* Ode to Joy (Beethoven) */
    {330,360},{330,360},{349,360},{392,360}, {392,360},{349,360},{330,360},{294,360},
    {262,360},{262,360},{294,360},{330,360}, {330,540},{294,180},{294,720},
    {330,360},{330,360},{349,360},{392,360}, {392,360},{349,360},{330,360},{294,360},
    {262,360},{262,360},{294,360},{330,360}, {294,540},{262,180},{262,720},
};
static const note_t twinkle[] = {   /* Twinkle Twinkle Little Star */
    {262,320},{262,320},{392,320},{392,320}, {440,320},{440,320},{392,640},
    {349,320},{349,320},{330,320},{330,320}, {294,320},{294,320},{262,640},
    {392,320},{392,320},{349,320},{349,320}, {330,320},{330,320},{294,640},
    {392,320},{392,320},{349,320},{349,320}, {330,320},{330,320},{294,640},
};
static const note_t scale[] = {     /* C-major scale up and down */
    {262,240},{294,240},{330,240},{349,240}, {392,240},{440,240},{494,240},{523,360},
    {523,240},{494,240},{440,240},{392,240}, {349,240},{330,240},{294,240},{262,420},
};

typedef struct { const char *name; const note_t *notes; int len; } song_t;
static const song_t songs[] = {
    { "Ode to Joy",     ode,     (int)(sizeof(ode) / sizeof(ode[0])) },
    { "Twinkle Star",   twinkle, (int)(sizeof(twinkle) / sizeof(twinkle[0])) },
    { "C Major Scale",  scale,   (int)(sizeof(scale) / sizeof(scale[0])) },
};
#define NSONGS ((int)(sizeof(songs) / sizeof(songs[0])))

static int   cur = -1;          /* selected song, or -1                          */
static int   note_idx;          /* next note to synthesise                       */
static int   playing;
static int   loop = 1;
static int   vis_phase;         /* visualiser animation counter                  */
static int   vis_freq;          /* freq of the note currently feeding the buffer */
static int16_t pcm[BATCH * 2];  /* synthesis scratch (stereo)                    */

/* hot rects (client-local) */
enum { H_PLAY = 1, H_STOP, H_NEXT, H_SONG0 };
typedef struct { int x, y, w, h, id; } mhot_t;
static mhot_t mhots[12];
static int    nmhot;

/* ---- synthesis ---------------------------------------------------------- */
static void synth_note(int freq, int frames, int off) {
    if (freq <= 0) {                                 /* a rest */
        for (int i = 0; i < frames; i++) { pcm[(off + i) * 2] = 0; pcm[(off + i) * 2 + 1] = 0; }
        return;
    }
    int half = SR / (freq * 2); if (half < 1) half = 1;
    int amp = 6500, lvl = amp, cnt = 0;
    int atk = frames / 16; if (atk < 1) atk = 1;     /* click-free attack/release */
    for (int i = 0; i < frames; i++) {
        if (++cnt >= half) { cnt = 0; lvl = -lvl; }
        int env = 256;
        if (i < atk)               env = 256 * i / atk;
        else if (i > frames - atk) env = 256 * (frames - i) / atk;
        int s = lvl * env / 256;
        pcm[(off + i) * 2] = (int16_t)s; pcm[(off + i) * 2 + 1] = (int16_t)s;
    }
}

/* Fill pcm with whole upcoming notes until ~BATCH frames; returns frames filled
 * (0 only when the song ends and loop is off). */
static int build_batch(void) {
    if (cur < 0) return 0;
    const song_t *s = &songs[cur];
    int filled = 0;
    while (filled < BATCH) {
        if (note_idx >= s->len) { if (loop) note_idx = 0; else break; }
        note_t nt = s->notes[note_idx];
        int nf = SR * nt.ms / 1000;
        if (nf <= 0) { note_idx++; continue; }
        if (filled == 0 && nf > BATCH) nf = BATCH;    /* clamp an over-long note  */
        else if (filled + nf > BATCH) break;          /* defer to the next batch  */
        synth_note(nt.freq, nf, filled);
        filled += nf;
        vis_freq = nt.freq;
        note_idx++;
    }
    return filled;
}

/* ---- transport ---------------------------------------------------------- */
static void music_play(int song) {
    if (song < 0 || song >= NSONGS) return;
    cur = song; note_idx = 0; playing = 1;
    audio_stop();
    int f = build_batch();
    if (f > 0) audio_play_async(pcm, f);
}
static void music_stop(void) { playing = 0; note_idx = 0; audio_stop(); }
static void music_toggle(void) {
    if (cur < 0) { music_play(0); return; }
    if (playing) { playing = 0; audio_stop(); }
    else         { playing = 1; }                     /* tick resumes from note_idx */
}
static void music_next(void) { music_play((cur < 0 ? 0 : (cur + 1) % NSONGS)); }

static void music_tick(window_t *w) {
    (void)w;
    if (!playing) return;
    if (!audio_pcm_ok()) { playing = 0; return; }
    if (!audio_busy()) {
        int f = build_batch();
        if (f > 0) audio_play_async(pcm, f);
        else playing = 0;                             /* finished, loop off */
    }
    vis_phase++;
}

/* ---- input -------------------------------------------------------------- */
static void music_key(window_t *w, char c) {
    (void)w;
    if (c == ' ')      music_toggle();
    else if (c == 'n' || c == 'N') music_next();
    else if (c == 's' || c == 'S') music_stop();
}
static void music_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nmhot; i++) {
        mhot_t *h = &mhots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        if      (h->id == H_PLAY) music_toggle();
        else if (h->id == H_STOP) music_stop();
        else if (h->id == H_NEXT) music_next();
        else if (h->id >= H_SONG0) music_play(h->id - H_SONG0);
        return;
    }
}

/* ---- draw --------------------------------------------------------------- */
static uint32_t lerp_demo(int i, int n);
static int mox, moy;
static void reg_hot(int x, int y, int w, int h, int id) {
    if (nmhot < 12) { mhots[nmhot].x = x - mox; mhots[nmhot].y = y - moy;
                      mhots[nmhot].w = w; mhots[nmhot].h = h; mhots[nmhot].id = id; nmhot++; }
}
static int btn(int x, int y, const char *label, int id, uint32_t bg) {
    int w = g_text_width(label, 1) + 22, h = 26;
    g_round(x, y, w, h, 6, bg, 255);
    g_text(x + 11, y + 6, label, 0xFFFFFF, 1);
    reg_hot(x, y, w, h, id);
    return w;
}

static void music_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    nmhot = 0; mox = cx; moy = cy;
    g_fill(cx, cy, cw, ch, 0x0E0E16);

    /* header */
    g_text(cx + 16, cy + 14, "Music", COL_TEXT, 2);
    if (!audio_pcm_ok())
        g_text(cx + 16, cy + 40, "No PCM audio device (launch QEMU with -device AC97).", COL_WARN, 1);
    else {
        const char *np = (cur >= 0) ? songs[cur].name : "Nothing playing";
        char line[64]; int p = 0;
        const char *pre = playing ? "Now playing: " : (cur >= 0 ? "Paused: " : "");
        for (int i = 0; pre[i] && p < 60; i++) line[p++] = pre[i];
        for (int i = 0; np[i] && p < 63; i++) line[p++] = np[i];
        line[p] = 0;
        g_text(cx + 16, cy + 42, line, COL_ACCENT, 1);
    }

    /* visualiser: bars that bounce while playing */
    int vy = cy + 66, vh = 54, bx0 = cx + 16, bw = cw - 32;
    g_fill(bx0, vy, bw, vh, 0x14141C);
    int bars = 24, gap = 3, w1 = (bw - (bars - 1) * gap) / bars;
    for (int i = 0; i < bars; i++) {
        int seed = (vis_phase * 5 + i * 37 + vis_freq) & 63;
        int amp = playing ? (12 + seed * (vh - 14) / 63) : 6;
        int bx = bx0 + i * (w1 + gap);
        uint32_t col = lerp_demo(i, bars);
        g_fill(bx, vy + vh - amp, w1, amp, col);
    }

    /* transport */
    int ty = vy + vh + 14, txp = cx + 16;
    txp += btn(txp, ty, playing ? "Pause" : "Play", H_PLAY, COL_ACCENT) + 8;
    txp += btn(txp, ty, "Stop", H_STOP, COL_PANEL_3) + 8;
    txp += btn(txp, ty, "Next", H_NEXT, COL_PANEL_3) + 8;

    /* song list */
    int ly = ty + 40;
    g_text(cx + 16, ly, "Songs", COL_TEXT_DIM, 1); ly += 20;
    for (int i = 0; i < NSONGS; i++) {
        int rh = 28, rx = cx + 16, rw = cw - 32;
        int sel = (i == cur);
        g_round(rx, ly, rw, rh, 6, sel ? 0x223046 : COL_PANEL_2, 255);
        g_text(rx + 12, ly + 8, songs[i].name, sel ? COL_ACCENT : COL_TEXT, 1);
        if (sel && playing) g_text(rx + rw - 24, ly + 8, ">", COL_ACCENT, 1);
        reg_hot(rx, ly, rw, rh, H_SONG0 + i);
        ly += rh + 6;
    }
}

/* a soft accent->panel gradient for the visualiser bars */
static uint32_t lerp_demo(int i, int n) {
    uint32_t a = COL_ACCENT, b = 0x4060A0;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int rr = ar + (br - ar) * i / n, rg = ag + (bg - ag) * i / n, rb = ab + (bb - ab) * i / n;
    return ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

void music_app_init(void) {
    cur = -1; note_idx = 0; playing = 0; vis_phase = 0; vis_freq = 0;
    window_t *win = gui_add_window("Music", 460, 460, 0xE85AB0, ICON_MUSIC);
    if (!win) return;
    win->draw  = music_draw;
    win->key   = music_key;
    win->click = music_click;
    win->tick  = music_tick;
    win->min_w = 360; win->min_h = 360;
    win->x = 300; win->y = 90;
}
