#ifndef TAIKO_ITAIKO_PROTOCOL_H
#define TAIKO_ITAIKO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define ITAIKO_PROTOCOL_SETTING_COUNT 48

typedef struct {
    char line[160];
    size_t line_len;
    uint16_t values[ITAIKO_PROTOCOL_SETTING_COUNT];
    uint64_t valid;
    char version[32];
    char edition[32];
    char mode[24];
    uint32_t version_seen_tick;
    int started;
    int saw_version;
    int saw_edition;
} itaiko_protocol_parser_t;

void itaiko_protocol_parser_reset(itaiko_protocol_parser_t *parser);
void itaiko_protocol_parser_feed(itaiko_protocol_parser_t *parser,
                                 const uint8_t *data, size_t length,
                                 uint32_t now_tick);
int itaiko_protocol_parser_complete(const itaiko_protocol_parser_t *parser,
                                    uint32_t now_tick);

int itaiko_protocol_parse_pairs(const char *pairs,
                                uint16_t values[ITAIKO_PROTOCOL_SETTING_COUNT],
                                uint64_t *valid_out);
int itaiko_protocol_setting_allowed(unsigned key);
unsigned itaiko_protocol_setting_max(unsigned key);

#endif
