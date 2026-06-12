#include "card_issuer.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "debug.h"
#include "http_client.h"
#include "runtime.h"

#define CARD_ISSUER_PATH "/api/zucchini/cards"

static int token_valid_for_header(const char *token) {
    if (!token || !token[0])
        return 0;
    for (const char *p = token; *p; p++) {
        if (*p == '\r' || *p == '\n')
            return 0;
    }
    return 1;
}

static int parse_card_code(const http_response_t *resp, char out_code21[21]) {
    if (!resp || !resp->body || resp->body_len < 20)
        return 0;
    for (int i = 0; i < 20; i++) {
        unsigned char c = resp->body[i];
        if (c < '0' || c > '9')
            return 0;
        out_code21[i] = (char)c;
    }
    for (size_t i = 20; i < resp->body_len; i++) {
        unsigned char c = resp->body[i];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            return 0;
    }
    out_code21[20] = 0;
    return 1;
}

int card_issuer_create(char out_code21[21]) {
    if (!out_code21)
        return -1;
    out_code21[0] = 0;

    const char *token = g_cfg.zucchini_api_token[0]
        ? g_cfg.zucchini_api_token
        : TAIKO_ZUCCHINI_API_TOKEN;
    if (!token_valid_for_header(token)) {
        dbg_print("[cards] card issuer token missing/invalid\n");
        return -2;
    }
    if (!g_cfg.online_redirect_host[0]) {
        dbg_print("[cards] card issuer host missing\n");
        return -3;
    }

    char headers[256];
    int hn = snprintf(headers, sizeof headers,
                      "Authorization: Bearer %s\r\n"
                      "Accept: text/plain\r\n",
                      token);
    if (hn <= 0 || hn >= (int)sizeof headers) {
        dbg_print("[cards] card issuer header overflow\n");
        return -4;
    }

    http_response_t resp;
    int port = g_cfg.online_redirect_port ? (int)g_cfg.online_redirect_port : 443;
    int rc = http_request_direct("POST",
                                 g_cfg.online_redirect_host,
                                 port,
                                 CARD_ISSUER_PATH,
                                 headers,
                                 (size_t)hn,
                                 NULL,
                                 0,
                                 &resp);
    if (rc != 0) {
        dbg_print("[cards] card issuer request failed\n");
        dbg_print_hex32("[cards] http rc", (uint32_t)rc);
        return -5;
    }

    if (resp.status != 201 || !parse_card_code(&resp, out_code21)) {
        dbg_print("[cards] card issuer bad response\n");
        dbg_print_hex32("[cards] status", (uint32_t)resp.status);
        http_response_free(&resp);
        out_code21[0] = 0;
        return -6;
    }

    http_response_free(&resp);
    return 0;
}
