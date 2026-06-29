#include <stdint.h>
#include <stddef.h>
#include "audio.h"
#include "hw.h"
#include "io.h"
#include "dma.h"
#include "pcspk.h"
#include "pit.h"
#include "hpet.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  AC'97 audio (Intel 82801 / QEMU AC97). Two I/O BARs: BAR0 = mixer (NAM),
 *  BAR1 = bus-master (NABM). We program the PCM-out engine with a buffer
 *  descriptor list and stream 48 kHz/16-bit/stereo samples by DMA.
 * ===========================================================================*/

/* NAM mixer registers (BAR0). */
#define NAM_RESET        0x00
#define NAM_MASTER_VOL   0x02
#define NAM_PCM_VOL      0x18
#define NAM_EXT_AUDIO    0x28
#define NAM_PCM_RATE     0x2C

/* NABM bus-master, PCM-out box (BAR1 + 0x10). */
#define PO_BDBAR         0x10
#define PO_CIV           0x14
#define PO_LVI           0x15
#define PO_SR            0x16
#define PO_PICB          0x18
#define PO_CR            0x1B
#define GLOB_CNT         0x2C
#define GLOB_STA         0x30

#define CR_RPBM          0x01     /* run/pause bus master */
#define CR_RR            0x02     /* reset registers      */
#define SR_DCH           0x01     /* DMA controller halted */

#define BDL_ENTRIES      32
#define SAMPLE_RATE      48000

struct bdl_entry { uint32_t addr; uint16_t samples; uint16_t flags; } __attribute__((packed));

static struct {
    int      present;
    int      is_hda;
    uint16_t nam, nabm;          /* I/O port bases */
    struct dma_buf bdl;          /* buffer descriptor list */
    struct dma_buf pcm;          /* sample buffer (up to ~1s) */
    int      volume;             /* 0..100 */
    const char *name;
} a;

int  audio_present(void) { return 1; }                /* pcspk fallback is always there */
const char *audio_name(void) { return a.name ? a.name : "PC speaker"; }
int  audio_volume(void) { return a.present ? a.volume : 100; }

static void nam_w(uint8_t reg, uint16_t v) { outw((uint16_t)(a.nam + reg), v); }
static uint16_t glob_sta(void) { return inw((uint16_t)(a.nabm + GLOB_STA)); }

void audio_set_volume(int percent) {
    if (percent < 0) percent = 0; if (percent > 100) percent = 100;
    a.volume = percent;
    if (!a.present || a.is_hda) return;
    /* AC97 attenuation: 0 = loudest, 0x3F = quietest, per channel; bit15 mute. */
    uint16_t att = (uint16_t)((100 - percent) * 0x1F / 100);
    uint16_t v = (uint16_t)((att << 8) | att);
    if (percent == 0) v |= 0x8000;
    nam_w(NAM_MASTER_VOL, v);
    nam_w(NAM_PCM_VOL, v);
}

/* Program the BDL for the first `frames` stereo frames in a.pcm and start the
 * DMA engine. Returns without waiting (the buffer plays in the background). */
static void ac97_start_buffer(uint32_t frames) {
    struct bdl_entry *bdl = (struct bdl_entry *)a.bdl.virt;
    uint32_t per = 0xFFFE / 2;                 /* max stereo frames per descriptor */
    uint32_t off = 0, n = 0;
    for (n = 0; n < BDL_ENTRIES && off < frames; n++) {
        uint32_t f = frames - off;
        if (f > per) f = per;
        bdl[n].addr    = (uint32_t)(a.pcm.phys + off * 4);
        bdl[n].samples = (uint16_t)(f * 2);    /* 16-bit sample count */
        bdl[n].flags   = (n == 0) ? 0 : 0;     /* IOC off; we poll DCH */
        off += f;
    }
    if (n == 0) return;
    bdl[n - 1].flags = 0x4000;                 /* BUP on the final descriptor */

    outb((uint16_t)(a.nabm + PO_CR), CR_RR);   /* reset PCM-out engine */
    while (inb((uint16_t)(a.nabm + PO_CR)) & CR_RR) { }
    outl((uint16_t)(a.nabm + PO_BDBAR), (uint32_t)a.bdl.phys);
    outb((uint16_t)(a.nabm + PO_LVI), (uint8_t)(n - 1));
    outw((uint16_t)(a.nabm + PO_SR), 0x1C);    /* clear status bits */
    outb((uint16_t)(a.nabm + PO_CR), CR_RPBM); /* run */
}

/* Stream the first `frames` stereo frames already sitting in a.pcm. Blocking. */
static void ac97_play_buffer(uint32_t frames) {
    ac97_start_buffer(frames);
    /* wait until the DMA engine halts (buffer drained) */
    for (uint64_t t = 0; t < 200000000ull; t++) {
        if (inw((uint16_t)(a.nabm + PO_SR)) & SR_DCH) break;
    }
    outb((uint16_t)(a.nabm + PO_CR), 0);
}

/* Non-blocking playback: copy PCM into the device buffer, start DMA, return.
 * Poll audio_busy() to know when it has drained, then push the next chunk. */
void audio_play_async(const int16_t *interleaved, uint32_t frames) {
    if (!a.present || a.is_hda || !interleaved || !frames) return;
    uint32_t cap = (uint32_t)(a.pcm.size / 4);
    if (frames > cap) frames = cap;
    memcpy(a.pcm.virt, interleaved, frames * 4);
    ac97_start_buffer(frames);
}

/* 1 while the PCM-out DMA engine is still playing a buffer. */
int audio_busy(void) {
    if (!a.present || a.is_hda) return 0;
    return (inw((uint16_t)(a.nabm + PO_SR)) & SR_DCH) ? 0 : 1;
}

/* Stop any in-progress playback immediately. */
void audio_stop(void) {
    if (!a.present || a.is_hda) return;
    outb((uint16_t)(a.nabm + PO_CR), 0);
}

/* Is real PCM streaming available (AC97 claimed, not the HDA/speaker fallback)? */
int audio_pcm_ok(void) { return a.present && !a.is_hda; }

void audio_play_pcm(const int16_t *interleaved, uint32_t frames) {
    if (!a.present || a.is_hda) return;
    uint32_t cap = (uint32_t)(a.pcm.size / 4);
    if (frames > cap) frames = cap;
    memcpy(a.pcm.virt, interleaved, frames * 4);
    ac97_play_buffer(frames);
}

void audio_tone(uint32_t freq, uint32_t ms) {
    if (!a.present || a.is_hda || freq == 0) {
        /* fallback: PC speaker */
        pcspk_tone(freq);
        uint64_t end = pit_ticks() + ms;
        while (pit_ticks() < end) __asm__ volatile("hlt");
        pcspk_off();
        return;
    }
    uint32_t frames = (SAMPLE_RATE * ms) / 1000;
    uint32_t cap = (uint32_t)(a.pcm.size / 4);
    if (frames > cap) frames = cap;

    /* integer square wave, ~25% amplitude to avoid clipping harshness */
    int16_t *buf = (int16_t *)a.pcm.virt;
    uint32_t half = freq ? (SAMPLE_RATE / (freq * 2)) : SAMPLE_RATE;
    if (half == 0) half = 1;
    int16_t amp = 8000;
    int16_t lvl = amp;
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (++cnt >= half) { cnt = 0; lvl = (int16_t)-lvl; }
        buf[i * 2]     = lvl;
        buf[i * 2 + 1] = lvl;
    }
    ac97_play_buffer(frames);
}

void audio_init(void) {
    a.volume = 80;
    a.name = "PC speaker";

    struct pci_dev pd;
    int have = 0;

    /* Intel HDA (class 0x04 subclass 0x03) -- detect/report; PCM path is AC97. */
    if (pci_find_by_class(0x04, 0x03, &pd)) {
        a.is_hda = 1;
        a.name = "Intel HDA";
        pci_enable_bus_master(&pd);
        a.present = 1;
        kprintf("[audio] Intel HDA detected (%x:%x); using PC speaker for tones\n",
                pd.vendor, pd.device);
        /* keep scanning for an AC97 we can actually stream through */
    }

    /* AC'97 (class 0x04 subclass 0x01, or Intel 8086:2415). */
    if (pci_find_by_class(0x04, 0x01, &pd) || pci_find_by_id(0x8086, 0x2415, &pd)) {
        have = 1;
    }
    if (!have) {
        if (!a.present) kprintf("[audio] no PCI codec; PC speaker only\n");
        return;
    }

    struct pci_bar nam, nabm;
    if (pci_bar(&pd, 0, &nam) != 0 || pci_bar(&pd, 1, &nabm) != 0) {
        kprintf("[audio] AC97 BAR decode failed\n");
        return;
    }
    a.nam  = nam.is_mmio  ? 0 : nam.port;
    a.nabm = nabm.is_mmio ? 0 : nabm.port;
    if (!a.nam || !a.nabm) { kprintf("[audio] AC97 expects I/O BARs\n"); return; }

    pci_enable_bus_master(&pd);

    if (dma_alloc(BDL_ENTRIES * sizeof(struct bdl_entry), &a.bdl) != 0) return;
    if (dma_alloc(256 * 1024, &a.pcm) != 0) return;   /* ~1.3s @ 48k stereo */

    /* cold reset + wait for primary codec ready */
    outl((uint16_t)(a.nabm + GLOB_CNT), 0x00000002);
    for (int t = 0; t < 1000000 && !(glob_sta() & 0x100); t++) __asm__ volatile("pause");
    nam_w(NAM_RESET, 0xFFFF);

    a.present = 1;
    a.is_hda = 0;
    a.name = "AC97";
    audio_set_volume(a.volume);

    kprintf("[ok] AC97 audio %x:%x (NAM=0x%x NABM=0x%x), 48kHz stereo\n",
            pd.vendor, pd.device, a.nam, a.nabm);
}
