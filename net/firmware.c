#include <stdint.h>
#include "firmware.h"
#include "string.h"

/* ===========================================================================
 *  In-RAM firmware registry. No real vendor blobs are linked into BoltOS yet,
 *  so this table starts empty and firmware_request() honestly returns -1; a
 *  driver that needs firmware must fail probe rather than run a dead radio.
 *  When an ath9k-htc blob is added (linked .rodata array or loaded from the FS),
 *  a single firmware_provide() at init makes it visible with no driver change.
 * ===========================================================================*/
#define FW_MAX 8

static struct fw_entry { const char *name; struct firmware fw; } table[FW_MAX];
static int fw_count;

void firmware_provide(const char *name, const uint8_t *data, uint32_t size) {
    if (fw_count >= FW_MAX) return;
    table[fw_count].name    = name;
    table[fw_count].fw.data = data;
    table[fw_count].fw.size = size;
    fw_count++;
}

int firmware_request(const char *name, struct firmware *fw) {
    for (int i = 0; i < fw_count; i++)
        if (strcmp(table[i].name, name) == 0) { *fw = table[i].fw; return 0; }
    return -1;
}
