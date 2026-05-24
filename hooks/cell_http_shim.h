#ifndef CELL_HTTP_SHIM_H
#define CELL_HTTP_SHIM_H

#include <stdint.h>
#include <stddef.h>

/* SPRX-side reimplementation of the cellHttp / cellHttps / cellSsl /
 * cellHttpUtil APIs the game imports. Types mirror Sony's SDK
 * headers (cell/http/util.h + cell/http/client.h + cell/ssl/cert.h)
 * so the ABI lines up byte-for-byte with the game's callers.
 *
 * Handle types (CellHttpClientId / CellHttpTransId) are pointer-shaped
 * in Sony's headers. Internally we treat them as small integers
 * (index + 1) cast through uintptr_t, so 0/NULL stays sentinel-invalid.
 *
 * All functions return 0 on success and a negative value (caller-
 * opaque, used by the game's state machine to retry) on failure.
 */

/* Match Sony's CellHttpUri layout (5 char* + u32 port + 4 reserved).
 * Size 28 bytes on PPC32 ABI. Sony's PPU SDK builds in 32-bit pointer
 * mode for game code, so the pointers here are 4 bytes each. */
typedef struct CellHttpUri {
    const char *scheme;
    const char *hostname;
    const char *username;
    const char *password;
    const char *path;
    uint32_t    port;
    uint8_t     reserved[4];
} CellHttpUri;

typedef struct CellHttpHeader {
    const char *name;
    const char *value;
} CellHttpHeader;

/* All 23 shim entry points. http_hook.c forwards from its trampoline
 * stubs into these. */

int sh_cellHttpInit(void *pool, uint32_t poolSize);
int sh_cellSslInit (void *pool, uint32_t poolSize);
int sh_cellHttpsInit(uint32_t caCertNum, const void *caList);

int sh_cellHttpCreateClient (uintptr_t *clientId);
int sh_cellHttpDestroyClient(uintptr_t  clientId);
int sh_cellHttpClientSetSslCallback     (uintptr_t clientId, void *cb, void *userArg);
int sh_cellHttpClientSetConnTimeout     (uintptr_t clientId, int64_t usec);
int sh_cellHttpClientSetKeepAlive       (uintptr_t clientId, int enable);
int sh_cellHttpClientCloseAllConnections(uintptr_t clientId);

int sh_cellHttpCreateTransaction (uintptr_t *transId, uintptr_t clientId,
                                  const char *method, const CellHttpUri *uri);
int sh_cellHttpDestroyTransaction(uintptr_t transId);
int sh_cellHttpRequestSetHeader        (uintptr_t transId, const CellHttpHeader *header);
int sh_cellHttpRequestSetContentLength (uintptr_t transId, uint64_t totalSize);
int sh_cellHttpRequestGetAllHeaders    (uintptr_t transId, CellHttpHeader **headers,
                                        uint32_t *items, void *pool,
                                        uint32_t poolSize, uint32_t *required);
int sh_cellHttpSendRequest             (uintptr_t transId, const void *buf,
                                        uint64_t size, uint64_t *sent);
int sh_cellHttpRecvResponse            (uintptr_t transId, void *buf,
                                        uint64_t size, uint64_t *recvd);
int sh_cellHttpResponseGetStatusCode   (uintptr_t transId, int32_t *code);
int sh_cellHttpResponseGetContentLength(uintptr_t transId, uint64_t *length);
int sh_cellHttpTransactionCloseConnection(uintptr_t transId);
int sh_cellHttpTransactionAbortConnection(uintptr_t transId);

int sh_cellHttpUtilParseUri(CellHttpUri *uri, const char *str,
                            void *pool, uint32_t size, uint32_t *required);

int sh_cellSslCertGetNotBefore(const void *cert, uint64_t *tick);
int sh_cellSslCertGetNotAfter (const void *cert, uint64_t *tick);

#endif
