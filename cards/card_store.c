#include "card_store.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "cfg_file.h"
#include "debug.h"

#define CARDS_PATH      "/dev_hdd0/plugins/taiko/cards.cfg"
#define CARDS_TMP_PATH  "/dev_hdd0/plugins/taiko/cards.cfg.tmp"

typedef struct {
    char label[CARD_LABEL_CAP];
    char code[CARD_CODE_LEN + 1];
} card_t;

static card_t g_cards[CARD_STORE_MAX];
static int    g_count;
static int    g_loaded;

static int code_valid(const char *code) {
    if (!code) return 0;
    for (int i = 0; i < CARD_CODE_LEN; i++) {
        if (code[i] < '0' || code[i] > '9')
            return 0;
    }
    return code[CARD_CODE_LEN] == '\0';
}

static void copy_label(char *dst, const char *src, int index) {
    int n = 0;
    if (src) {
        for (; src[n] && n < CARD_LABEL_CAP - 1; n++) {
            char c = src[n];
            /* keep the file format simple: no newlines/'=' in labels. */
            if (c == '\n' || c == '\r' || c == '=')
                c = ' ';
            dst[n] = c;
        }
    }
    if (n == 0) {
        /* Auto name: "Card N". */
        int v = index + 1;
        char tmp[8];
        int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v && t < (int)sizeof tmp) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        const char *p = "Card ";
        while (*p && n < CARD_LABEL_CAP - 1) dst[n++] = *p++;
        while (t > 0 && n < CARD_LABEL_CAP - 1) dst[n++] = tmp[--t];
    }
    dst[n] = '\0';
}

/* cfg parser handler: in the [cards] section each entry is `code = label`. */
static void handle_cards(const char *key, const char *value, void *u) {
    (void)u;
    if (g_count >= CARD_STORE_MAX)
        return;
    if (!code_valid(key))
        return;
    card_t *c = &g_cards[g_count];
    memcpy(c->code, key, CARD_CODE_LEN + 1);
    copy_label(c->label, value, g_count);
    g_count++;
}

void card_store_load(void) {
    g_count = 0;
    g_loaded = 1;

    static char buf[8192];
    uint64_t got = 0;
    if (!cfg_file_read(CARDS_PATH, buf, sizeof buf - 1, &got) || got == 0)
        return;
    buf[got] = 0;

    const cfg_section_t sections[] = {
        {"cards", handle_cards, NULL},
    };
    cfg_file_parse(buf, (size_t)got, sections,
                   sizeof sections / sizeof sections[0]);
}

static void card_store_save(void) {
    int fd = cfg_file_open_write(CARDS_TMP_PATH);
    if (fd < 0) {
        dbg_print("[cards] open-for-write failed\n");
        return;
    }

    cfg_file_write_str(fd,
        "# Saved banapassport cards (shared across Taiko builds).\n"
        "# Format under [cards]: <20-digit-code> = <label>\n\n[cards]\n");

    for (int i = 0; i < g_count; i++) {
        cfg_file_write_str(fd, g_cards[i].code);
        cfg_file_write_str(fd, " = ");
        cfg_file_write_str(fd, g_cards[i].label);
        cfg_file_write_str(fd, "\n");
    }
    cfg_file_close(fd);

    cellFsUnlink(CARDS_PATH);
    if (cellFsRename(CARDS_TMP_PATH, CARDS_PATH) != CELL_FS_SUCCEEDED) {
        cellFsUnlink(CARDS_TMP_PATH);
        dbg_print("[cards] rename failed\n");
    }
}

int card_store_count(void) {
    if (!g_loaded) card_store_load();
    return g_count;
}

const char *card_store_label(int i) {
    if (i < 0 || i >= g_count) return NULL;
    return g_cards[i].label;
}

const char *card_store_code(int i) {
    if (i < 0 || i >= g_count) return NULL;
    return g_cards[i].code;
}

int card_store_add(const char *label, const char *code) {
    if (!g_loaded) card_store_load();
    if (g_count >= CARD_STORE_MAX) return 0;
    if (!code_valid(code)) return 0;

    card_t *c = &g_cards[g_count];
    memcpy(c->code, code, CARD_CODE_LEN + 1);
    copy_label(c->label, label, g_count);
    g_count++;

    card_store_save();
    return 1;
}

int card_store_remove(int i) {
    if (i < 0 || i >= g_count) return 0;
    for (int j = i; j < g_count - 1; j++)
        g_cards[j] = g_cards[j + 1];
    g_count--;
    card_store_save();
    return 1;
}
