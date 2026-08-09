#include "itaiko_protocol.h"

#include <stdio.h>
#include <string.h>

#define ITAIKO_LEGACY_SETTLE_TICKS 100u

static void copy_text(char *out, size_t capacity, const char *text) {
    size_t used = 0;
    if (!capacity)
        return;
    while (text[used] && used + 1 < capacity) {
        unsigned char c = (unsigned char)text[used];
        out[used] = c >= 0x20 && c <= 0x7e ? (char)c : '?';
        used++;
    }
    out[used] = '\0';
}

static int parse_pair(const char *text, unsigned *key_out,
                      unsigned *value_out) {
    const char *p = text;
    unsigned key = 0;
    unsigned value = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        if (key > 1000u)
            return 0;
        key = key * 10u + (unsigned)(*p++ - '0');
        digits++;
    }
    if (!digits || *p++ != ':')
        return 0;
    digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (value > 65535u / 10u)
            return 0;
        value = value * 10u + (unsigned)(*p++ - '0');
        if (value > 65535u)
            return 0;
        digits++;
    }
    if (!digits || *p)
        return 0;
    *key_out = key;
    *value_out = value;
    return 1;
}

void itaiko_protocol_parser_reset(itaiko_protocol_parser_t *parser) {
    memset(parser, 0, sizeof(*parser));
    parser->version_seen_tick = UINT32_MAX;
}

static void accept_line(itaiko_protocol_parser_t *parser, uint32_t now_tick) {
    unsigned key;
    unsigned value;

    parser->line[parser->line_len] = '\0';
    if (parse_pair(parser->line, &key, &value) &&
        key < ITAIKO_PROTOCOL_SETTING_COUNT) {
        if (key == 0) {
            uint16_t first = (uint16_t)value;
            itaiko_protocol_parser_reset(parser);
            parser->started = 1;
            parser->values[0] = first;
            parser->valid = 1ull;
            return;
        }
        if (parser->started) {
            parser->values[key] = (uint16_t)value;
            parser->valid |= 1ull << key;
        }
    } else if (parser->started && strncmp(parser->line, "Mode:", 5) == 0) {
        copy_text(parser->mode, sizeof(parser->mode), parser->line + 5);
    } else if (parser->started &&
               strncmp(parser->line, "Version:", 8) == 0) {
        copy_text(parser->version, sizeof(parser->version), parser->line + 8);
        parser->saw_version = 1;
        parser->version_seen_tick = now_tick;
    } else if (parser->started &&
               strncmp(parser->line, "Edition:", 8) == 0) {
        copy_text(parser->edition, sizeof(parser->edition), parser->line + 8);
        parser->saw_edition = 1;
    }
}

void itaiko_protocol_parser_feed(itaiko_protocol_parser_t *parser,
                                 const uint8_t *data, size_t length,
                                 uint32_t now_tick) {
    for (size_t i = 0; i < length; i++) {
        uint8_t c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            accept_line(parser, now_tick);
            parser->line_len = 0;
            continue;
        }
        if (parser->line_len + 1 < sizeof(parser->line))
            parser->line[parser->line_len++] =
                c >= 0x20 && c <= 0x7e ? (char)c : '.';
        else
            parser->line_len = 0;
    }
}

int itaiko_protocol_parser_complete(const itaiko_protocol_parser_t *parser,
                                    uint32_t now_tick) {
    const uint64_t required = (1ull << 18) - 1ull;
    if ((parser->valid & required) != required || !parser->saw_version)
        return 0;
    if (parser->saw_edition)
        return 1;
    return parser->version_seen_tick != UINT32_MAX &&
           now_tick - parser->version_seen_tick >= ITAIKO_LEGACY_SETTLE_TICKS;
}

int itaiko_protocol_setting_allowed(unsigned key) {
    return key <= 17u || key == 46u;
}

unsigned itaiko_protocol_setting_max(unsigned key) {
    if (key <= 3u || (key >= 10u && key <= 17u))
        return 4095u;
    if (key == 9u)
        return 1u;
    if (key == 46u)
        return 50u;
    return 1000u;
}

int itaiko_protocol_parse_pairs(const char *pairs,
                                uint16_t values[ITAIKO_PROTOCOL_SETTING_COUNT],
                                uint64_t *valid_out) {
    char copy[256];
    char *saveptr = NULL;
    char *token;
    uint64_t valid = 0;
    size_t length;

    if (!pairs || !pairs[0])
        return 0;
    length = strlen(pairs);
    if (length >= sizeof(copy))
        return 0;
    memcpy(copy, pairs, length + 1);
    memset(values, 0, sizeof(uint16_t) * ITAIKO_PROTOCOL_SETTING_COUNT);

    token = strtok_r(copy, " \r\n", &saveptr);
    while (token) {
        unsigned key;
        unsigned value;
        if (!parse_pair(token, &key, &value) ||
            !itaiko_protocol_setting_allowed(key) ||
            value > itaiko_protocol_setting_max(key) ||
            (valid & (1ull << key)))
            return 0;
        values[key] = (uint16_t)value;
        valid |= 1ull << key;
        token = strtok_r(NULL, " \r\n", &saveptr);
    }
    if (!valid)
        return 0;
    *valid_out = valid;
    return 1;
}
