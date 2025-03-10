#include <time.h>
#include <stdbool.h>
#include <string.h>

#include "settings.h"

#include "handler_cloud.h"
#include "http/http_client.h"

#include "cloud_request.h"
#include "proto/toniebox.pb.freshness-check.fc-request.pb-c.h"
#include "proto/toniebox.pb.freshness-check.fc-response.pb-c.h"

#define PROX_STATUS_IDLE 0
#define PROX_STATUS_CONN 1
#define PROX_STATUS_HEAD 2
#define PROX_STATUS_BODY 3
#define PROX_STATUS_DONE 4

typedef enum
{
    NONE = 0,
    V1_TIME,
    V1_OTA,
    V1_CLAIM,
    V2_CONTENT,
    V1_FRESHNESS_CHECK,
    V1_LOG
} cloudapi_t;

typedef enum
{
    OTA_FIRMWARE_PD = 2,
    OTA_FIRMWARE_EU = 3,
    OTA_SERVICEPACK_CC3200 = 4,
    OTA_HTML_CONFIG = 5,
    OTA_SFX_BIN = 6,
} cloudapi_ota_t;

typedef struct
{
    const char_t *uri;
    cloudapi_t api;
    char_t *buffer;
    size_t bufferPos;
    size_t bufferLen;
    uint32_t status;
    FsFile *file;
    tonie_info_t tonieInfo;
    HttpConnection *connection;
} cbr_ctx_t;

static void setTonieboxSettings(TonieFreshnessCheckResponse *freshResp);
static void setTonieboxSettings(TonieFreshnessCheckResponse *freshResp)
{
    freshResp->max_vol_spk = Settings.toniebox.max_vol_spk;
    freshResp->max_vol_hdp = Settings.toniebox.max_vol_hdp;
    freshResp->slap_en = Settings.toniebox.slap_enabled;
    freshResp->slap_dir = Settings.toniebox.slap_back_left;
    freshResp->led = Settings.toniebox.led;
}

static void cbrCloudResponsePassthrough(void *src_ctx, void *cloud_ctx);
static void cbrCloudHeaderPassthrough(void *src_ctx, void *cloud_ctx, const char *header, const char *value);
static void cbrCloudBodyPassthrough(void *src_ctx, void *cloud_ctx, const char *payload, size_t length, error_t error);
static void cbrCloudServerDiscoPassthrough(void *src_ctx, void *cloud_ctx);

static void strupr(char input[]);

static void cbrCloudResponsePassthrough(void *src_ctx, void *cloud_ctx)
{
    cbr_ctx_t *ctx = (cbr_ctx_t *)src_ctx;
    char line[128];

    // This is fine: https://www.youtube.com/watch?v=0oBx7Jg4m-o
    osSprintf(line, "HTTP/%u.%u %u This is fine\r\n", MSB(ctx->connection->response.version), LSB(ctx->connection->response.version), ctx->connection->response.statusCode);
    httpSend(ctx->connection, line, osStrlen(line), HTTP_FLAG_DELAY);
    ctx->status = PROX_STATUS_CONN;
}

static void cbrCloudHeaderPassthrough(void *src_ctx, void *cloud_ctx, const char *header, const char *value)
{
    cbr_ctx_t *ctx = (cbr_ctx_t *)src_ctx;
    char line[128];

    if (header)
    {
        TRACE_INFO(">> cbrCloudHeaderPassthrough: %s = %s\r\n", header, value);
        osSprintf(line, "%s: %s\r\n", header, value);
    }
    else
    {
        TRACE_INFO(">> cbrCloudHeaderPassthrough: NULL\r\n");
        osStrcpy(line, "\r\n");
    }

    httpSend(ctx->connection, line, osStrlen(line), HTTP_FLAG_DELAY);
    ctx->status = PROX_STATUS_HEAD;
}

static bool fillCbrBodyCache(cbr_ctx_t *ctx, HttpClientContext *httpClientContext, const char *payload, size_t length)
{
    if (ctx->bufferPos == 0)
    {
        ctx->bufferLen = httpClientContext->bodyLen; // ctx->connection->response.contentLength;
        ctx->buffer = osAllocMem(ctx->bufferLen);
    }
    osMemcpy(&ctx->buffer[ctx->bufferPos], payload, length);
    ctx->bufferPos += length;
    return (ctx->bufferPos == ctx->bufferLen);
}

static void cbrCloudBodyPassthrough(void *src_ctx, void *cloud_ctx, const char *payload, size_t length, error_t error)
{
    cbr_ctx_t *ctx = (cbr_ctx_t *)src_ctx;
    HttpClientContext *httpClientContext = (HttpClientContext *)cloud_ctx;

    // TRACE_INFO(">> cbrCloudBodyPassthrough: %lu received\r\n", length);
    switch (ctx->api)
    {
    case V2_CONTENT:
        if (Settings.cloud.cacheContent && httpClientContext->statusCode == 200)
        {
            // TRACE_INFO(">> cbrCloudBodyPassthrough: %lu received\r\n", length);
            // TRACE_INFO(">> %s\r\n", ctx->uri);
            if (ctx->status == PROX_STATUS_HEAD)
            {
                /* URI is always "/v2/content/xxxxxxxxxx0304E0" where the x's are hex digits. length has to be fixed */
                TRACE_INFO(">> Start caching uri=%s\r\n", ctx->uri);
                // TODO detect partial downloads
                if (strlen(ctx->uri) < 28)
                {
                    TRACE_ERROR(">> ctx->uri is too short\r\n");
                    return;
                }
                char ruid[17];
                osStrncpy(ruid, &ctx->uri[12], sizeof(ruid));
                ruid[16] = 0;
                getContentPathFromCharRUID(ruid, ctx->tonieInfo.contentPath);
                char tmpPath[34];
                ctx->tonieInfo = getTonieInfo(ctx->tonieInfo.contentPath);
                osMemcpy(tmpPath, ctx->tonieInfo.contentPath, 30);
                tmpPath[29] = 0;
                osStrcat(tmpPath, ".tmp");
                tmpPath[20] = 0;
                fsCreateDir(tmpPath);
                tmpPath[20] = '/';
                ctx->file = fsOpenFile(tmpPath, FS_FILE_MODE_WRITE | FS_FILE_MODE_TRUNC);
            }
            if (length > 0)
            {
                error_t error = fsWriteFile(ctx->file, (void *)payload, length);
                if (error)
                    TRACE_ERROR(">> fsWriteFile Error: %u\r\n", error);
            }
            if (error == ERROR_END_OF_STREAM)
            {
                fsCloseFile(ctx->file);
                char tmpPath[34];
                osMemcpy(tmpPath, ctx->tonieInfo.contentPath, 30);
                tmpPath[29] = 0;
                osStrcat(tmpPath, ".tmp");
                fsDeleteFile(ctx->tonieInfo.contentPath);
                fsRenameFile(tmpPath, ctx->tonieInfo.contentPath);
                TRACE_INFO(">> Successfully cached %s\r\n", ctx->tonieInfo.contentPath);
            }
        }
        httpSend(ctx->connection, payload, length, HTTP_FLAG_DELAY);
        break;
    case V1_FRESHNESS_CHECK:
        if (Settings.toniebox.overrideCloud && length > 0 && fillCbrBodyCache(ctx, httpClientContext, payload, length))
        {
            TonieFreshnessCheckResponse *freshResp = tonie_freshness_check_response__unpack(NULL, ctx->bufferLen, (const uint8_t *)ctx->buffer);
            setTonieboxSettings(freshResp);
            size_t packSize = tonie_freshness_check_response__get_packed_size(freshResp);

            // TODO: Check if size is stable and this is obsolete
            // TODO Add live tonies here, too : freshResp.tonie_marked
            if (ctx->bufferLen < packSize)
            {
                TRACE_WARNING(">> cbrCloudBodyPassthrough V1_FRESHNESS_CHECK: %zu / %zu\r\n", ctx->bufferLen, packSize);
                osFreeMem(ctx->buffer);
                ctx->bufferLen = packSize;
                ctx->buffer = osAllocMem(ctx->bufferLen);
            }
            tonie_freshness_check_response__pack(freshResp, (uint8_t *)ctx->buffer);
            tonie_freshness_check_response__free_unpacked(freshResp, NULL);
            httpSend(ctx->connection, ctx->buffer, ctx->bufferLen, HTTP_FLAG_DELAY);
            osFreeMem(ctx->buffer);
        }
        else
        {
            httpSend(ctx->connection, payload, length, HTTP_FLAG_DELAY);
        }
        break;
    default:
        httpSend(ctx->connection, payload, length, HTTP_FLAG_DELAY);
        break;
    }
    ctx->status = PROX_STATUS_BODY;
}

static void cbrCloudServerDiscoPassthrough(void *src_ctx, void *cloud_ctx)
{
    cbr_ctx_t *ctx = (cbr_ctx_t *)src_ctx;
    TRACE_INFO(">> cbrCloudServerDiscoPassthrough\r\n");
    ctx->status = PROX_STATUS_DONE;
}

static req_cbr_t getCloudCbr(HttpConnection *connection, const char_t *uri, cloudapi_t api, cbr_ctx_t *ctx);

static req_cbr_t getCloudCbr(HttpConnection *connection, const char_t *uri, cloudapi_t api, cbr_ctx_t *ctx)
{
    ctx->uri = uri;
    ctx->api = api;
    ctx->buffer = NULL;
    ctx->bufferPos = 0;
    ctx->bufferLen = 0;
    ctx->status = PROX_STATUS_IDLE;
    ctx->connection = connection;

    req_cbr_t cbr = {
        .ctx = ctx,
        .response = &cbrCloudResponsePassthrough,
        .header = &cbrCloudHeaderPassthrough,
        .body = &cbrCloudBodyPassthrough,
        .disconnect = &cbrCloudServerDiscoPassthrough};

    return cbr;
}

void getContentPathFromCharRUID(char ruid[17], char contentPath[30])
{
    osSprintf(contentPath, "www/CONTENT/%.8s/%.8s", ruid, &ruid[8]);
    strupr(&contentPath[4]);
    contentPath[30] = '\0';
}

void getContentPathFromUID(uint64_t uid, char contentPath[30])
{
    uint16_t cuid[9];
    osSprintf((char *)cuid, "%016" PRIX64 "", uid);
    uint16_t cruid[9];
    for (uint8_t i = 0; i < 8; i++)
    {
        cruid[i] = cuid[7 - i];
    }
    cruid[8] = 0;
    getContentPathFromCharRUID((char *)cruid, contentPath);
}

tonie_info_t getTonieInfo(char contentPath[30])
{
    tonie_info_t tonieInfo;
    char contentPathDot[30 + 8]; //".nocloud" / ".live"
    osMemcpy(contentPathDot, contentPath, 30);
    osMemcpy(tonieInfo.contentPath, contentPath, 30);

    tonieInfo.exists = fsFileExists(contentPathDot);
    osStrcat(contentPathDot, ".nocloud");
    tonieInfo.nocloud = fsFileExists(contentPathDot);
    contentPathDot[29] = 0;
    osStrcat(contentPathDot, ".live");
    tonieInfo.live = fsFileExists(contentPathDot);

    return tonieInfo;
}

error_t httpWriteResponse(HttpConnection *connection, void *data, bool_t freeMemory)
{
    error_t error = httpWriteHeader(connection);
    if (error != NO_ERROR)
    {
        if (freeMemory)
            osFreeMem(data);
        TRACE_ERROR("Failed to send header\r\n");
        return error;
    }

    error = httpWriteStream(connection, data, connection->response.contentLength);
    if (freeMemory)
        if (freeMemory)
            osFreeMem(data);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Failed to send payload: %d\r\n", error);
        return error;
    }

    error = httpCloseStream(connection);
    if (error != NO_ERROR)
    {
        TRACE_ERROR("Failed to close: %d\r\n", error);
        return error;
    }

    return NO_ERROR;
}
void httpPrepareHeader(HttpConnection *connection, const void *contentType, size_t contentLength)
{
    httpInitResponseHeader(connection);
    connection->response.keepAlive = true;
    connection->response.chunkedEncoding = false;
    connection->response.contentType = contentType;
    connection->response.contentLength = contentLength;
}

error_t handleCloudTime(HttpConnection *connection, const char_t *uri)
{
    TRACE_INFO(" >> respond with current time\r\n");

    char response[32];

    if (!settings_get_bool("cloud.enabled") || !settings_get_bool("cloud.enableV1Time"))
    {
        sprintf(response, "%ld", time(NULL));
    }
    else
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, V1_TIME, &ctx);
        if (!cloud_request_get(NULL, 0, uri, NULL, &cbr))
        {
            return NO_ERROR;
        }
        else
        {
            sprintf(response, "%lu", time(NULL));
        }
    }

    httpPrepareHeader(connection, "text/plain; charset=utf-8", osStrlen(response));
    return httpWriteResponse(connection, response, false);
}

error_t handleCloudOTA(HttpConnection *connection, const char_t *uri)
{
    error_t ret = NO_ERROR;
    char *query = strdup(connection->request.queryString);
    char *localUri = strdup(uri);
    char *savelocalUri = localUri;
    char *filename = strtok_r(&localUri[8], "?", &savelocalUri);
    char *cv = strpbrk(query, "cv=");
    char *timestampTxt = cv ? strtok_r(&cv[3], "&", &cv) : NULL;

    uint8_t fileId = atoi(filename);
    (void)fileId;
    time_t timestamp = timestampTxt ? atoi(timestampTxt) : 0;

    char date_buffer[32] = "";
    struct tm tm_info;
    if (localtime_r(&timestamp, &tm_info) != NULL)
    {
        strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
    }

    TRACE_INFO(" >> OTA-Request for %u with timestamp %lu (%s)\r\n", fileId, timestamp, date_buffer);

    if (settings_get_bool("cloud.enabled") && settings_get_bool("cloud.enableV1Ota"))
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, V1_OTA, &ctx);
        cloud_request_get(NULL, 0, uri, NULL, &cbr);
    }
    else
    {
        httpPrepareHeader(connection, NULL, 0);
        connection->response.statusCode = 304; // No new firmware
        ret = httpWriteResponse(connection, NULL, false);
    }

    free(query);
    free(localUri);

    return ret;
}

bool checkCustomTonie(char *ruid, uint8_t *token)
{
    if (Settings.cloud.markCustomTagByPass)
    {
        bool tokenIsZero = TRUE;
        for (uint8_t i; i < 32; i++)
        {
            if (token[i] != 0)
            {
                tokenIsZero = FALSE;
                break;
            }
        }
        if (tokenIsZero)
        {
            TRACE_INFO("Found possible custom tonie by password\r\n");
            return true;
        }
    }
    if (Settings.cloud.markCustomTagByUid &&
        (ruid[15] != '0' || ruid[14] != 'e' || ruid[13] != '4' || ruid[12] != '0' || ruid[11] != '3' || ruid[10] != '0'))
    {
        TRACE_INFO("Found possible custom tonie by uid\r\n");
        return true;
    }
    return false;
}
void markCustomTonie(tonie_info_t *tonieInfo)
{
    char contentDir[22];         //".nocloud" / ".live"
    char contentPathDot[30 + 8]; //".nocloud" / ".live"
    osMemcpy(contentDir, tonieInfo->contentPath, 21);
    contentDir[21] = '\0';
    osMemcpy(contentPathDot, tonieInfo->contentPath, 30);
    osStrcat(contentPathDot, ".nocloud");
    fsCreateDir(contentDir);
    FsFile *file = fsOpenFile(contentPathDot, FS_FILE_MODE_WRITE | FS_FILE_MODE_CREATE);
    fsCloseFile(file);
    TRACE_INFO("Marked custom tonie with file %s\r\n", contentPathDot);
}

error_t handleCloudLog(HttpConnection *connection, const char_t *uri)
{
    if (settings_get_bool("cloud.enabled") && settings_get_bool("cloud.enableV1Log"))
    {
        cbr_ctx_t ctx;
        req_cbr_t cbr = getCloudCbr(connection, uri, V1_LOG, &ctx);
        cloud_request_get(NULL, 0, uri, NULL, &cbr);
    }
    return NO_ERROR;
}

error_t handleCloudClaim(HttpConnection *connection, const char_t *uri)
{
    char ruid[17];
    uint8_t *token = connection->private.authentication_token;

    osStrncpy(ruid, &uri[10], sizeof(ruid));
    ruid[16] = 0;

    if (osStrlen(ruid) != 16)
    {
        TRACE_WARNING(" >>  invalid URI\r\n");
    }
    TRACE_INFO(" >> client requested rUID %s\r\n", ruid);
    TRACE_INFO(" >> client authenticated with %02X%02X%02X%02X...\r\n", token[0], token[1], token[2], token[3]);

    tonie_info_t tonieInfo;
    getContentPathFromCharRUID(ruid, tonieInfo.contentPath);
    tonieInfo = getTonieInfo(tonieInfo.contentPath);

    if (!tonieInfo.nocloud && checkCustomTonie(ruid, token))
    {
        tonieInfo.nocloud = true;
        markCustomTonie(&tonieInfo);
    }

    if (!settings_get_bool("cloud.enabled") || !settings_get_bool("cloud.enableV1Claim") || tonieInfo.nocloud)
    {
        return NO_ERROR;
    }

    cbr_ctx_t ctx;
    req_cbr_t cbr = getCloudCbr(connection, uri, V1_CLAIM, &ctx);
    cloud_request_get(NULL, 0, uri, token, &cbr);
    return NO_ERROR;
}

static void strupr(char input[])
{
    for (uint16_t i = 0; input[i]; i++)
    {
        input[i] = toupper(input[i]);
    }
}

error_t handleCloudContent(HttpConnection *connection, const char_t *uri, bool_t noPassword)
{
    char ruid[17];
    uint8_t *token = connection->private.authentication_token;

    osStrncpy(ruid, &uri[12], sizeof(ruid));
    ruid[16] = 0;

    if (connection->request.Range.start != 0)
    {
        TRACE_INFO(" >> client requested partial download\r\n");
    }

    if (osStrlen(ruid) != 16)
    {
        TRACE_WARNING(" >>  invalid URI\r\n");
    }
    TRACE_INFO(" >> client requested rUID %s\r\n", ruid);
    TRACE_INFO(" >> client authenticated with %02X%02X%02X%02X...\r\n", token[0], token[1], token[2], token[3]);

    tonie_info_t tonieInfo;
    getContentPathFromCharRUID(ruid, tonieInfo.contentPath);
    tonieInfo = getTonieInfo(tonieInfo.contentPath);

    if (!tonieInfo.nocloud && !noPassword && checkCustomTonie(ruid, token))
    {
        tonieInfo.nocloud = true;
        markCustomTonie(&tonieInfo);
    }

    if (tonieInfo.exists)
    {
        connection->response.keepAlive = true;
        error_t error = httpSendResponse(connection, &tonieInfo.contentPath[4]);
        if (error)
        {
            TRACE_ERROR(" >> file %s not available or not send, error=%u...\r\n", tonieInfo.contentPath, error);
            return error;
        }
    }
    else
    {
        if (!settings_get_bool("cloud.enabled") || !settings_get_bool("cloud.enableV2Content") || tonieInfo.nocloud)
        {
            if (tonieInfo.nocloud)
            {
                TRACE_INFO("Content marked as no cloud and no content locally available\r\n");
            }
            else
            {
                TRACE_INFO("No local content available and cloud access disabled\r\n");
            }
            httpPrepareHeader(connection, NULL, 0);
            connection->response.statusCode = 404;
            return httpWriteResponse(connection, NULL, false);
        }
        else
        {
            connection->response.keepAlive = true;
            cbr_ctx_t ctx;
            req_cbr_t cbr = getCloudCbr(connection, uri, V2_CONTENT, &ctx);
            cloud_request_get(NULL, 0, uri, token, &cbr);
        }
    }
    return NO_ERROR;
}
error_t handleCloudContentV1(HttpConnection *connection, const char_t *uri)
{
    return handleCloudContent(connection, uri, TRUE);
}
error_t handleCloudContentV2(HttpConnection *connection, const char_t *uri)
{
    if (connection->request.auth.found && connection->request.auth.mode == HTTP_AUTH_MODE_DIGEST)
    {
        return handleCloudContent(connection, uri, FALSE);
    }
    else
    {
        TRACE_WARNING("Missing auth for content v2: %s", uri);
    }
    return NO_ERROR;
}

error_t handleCloudFreshnessCheck(HttpConnection *connection, const char_t *uri)
{
    uint8_t data[BODY_BUFFER_SIZE];
    size_t size;
    if (BODY_BUFFER_SIZE <= connection->request.byteCount)
    {
        TRACE_ERROR("Body size %zu bigger than buffer size %i bytes", connection->request.byteCount, BODY_BUFFER_SIZE);
    }
    else
    {
        error_t error = httpReceive(connection, &data, BODY_BUFFER_SIZE, &size, 0x00);
        if (error != NO_ERROR)
        {
            TRACE_ERROR("httpReceive failed!");
            return error;
        }
        TRACE_INFO("Content (%zu of %zu)\n", size, connection->request.byteCount);
        TonieFreshnessCheckRequest *freshReq = tonie_freshness_check_request__unpack(NULL, size, (const uint8_t *)data);
        if (freshReq == NULL)
        {
            TRACE_ERROR("Unpacking freshness request failed!\r\n");
        }
        else
        {
            TRACE_INFO("Found %zu tonies:\n", freshReq->n_tonie_infos);
            TonieFreshnessCheckResponse freshResp = TONIE_FRESHNESS_CHECK_RESPONSE__INIT;
            freshResp.n_tonie_marked = 0;
            freshResp.tonie_marked = malloc(sizeof(uint64_t) * freshReq->n_tonie_infos);

            TonieFreshnessCheckRequest freshReqCloud = TONIE_FRESHNESS_CHECK_REQUEST__INIT;
            freshReqCloud.n_tonie_infos = 0;
            freshReqCloud.tonie_infos = malloc(sizeof(TonieFCInfo *) * freshReq->n_tonie_infos);

            for (uint16_t i = 0; i < freshReq->n_tonie_infos; i++)
            {
                struct tm tm_info;
                char date_buffer[32];
                bool_t custom = false;
                time_t unix_time = freshReq->tonie_infos[i]->audio_id;

                if (unix_time < 0x0e000000)
                {
                    sprintf(date_buffer, "special");
                }
                else
                {
                    /* custom tonies from TeddyBench have the audio id reduced by a constant */
                    if (unix_time < 0x50000000)
                    {
                        unix_time += 0x50000000;
                        custom = true;
                    }
                    if (localtime_r(&unix_time, &tm_info) == NULL)
                    {
                        sprintf(date_buffer, "(localtime failed)");
                    }
                    else
                    {
                        strftime(date_buffer, sizeof(date_buffer), "%Y-%m-%d %H:%M:%S", &tm_info);
                    }
                }
                tonie_info_t tonieInfo;
                getContentPathFromUID(freshReq->tonie_infos[i]->uid, tonieInfo.contentPath);
                tonieInfo = getTonieInfo(tonieInfo.contentPath);

                if (!tonieInfo.nocloud)
                {
                    freshReqCloud.tonie_infos[freshReqCloud.n_tonie_infos++] = freshReq->tonie_infos[i];
                }

                (void)custom;
                TRACE_INFO("  uid: %016" PRIX64 ", nocloud: %d, live: %d, audioid: %08X (%s%s)\n",
                           freshReq->tonie_infos[i]->uid,
                           tonieInfo.nocloud,
                           tonieInfo.live,
                           freshReq->tonie_infos[i]->audio_id,
                           date_buffer,
                           custom ? ", custom" : "");
                if (tonieInfo.live)
                {
                    freshResp.tonie_marked[freshResp.n_tonie_marked++] = freshReq->tonie_infos[i]->uid;
                }
            }

            if (settings_get_bool("cloud.enabled") && settings_get_bool("cloud.enableV1FreshnessCheck"))
            {
                size_t dataLen = tonie_freshness_check_request__get_packed_size(&freshReqCloud);
                tonie_freshness_check_request__pack(&freshReqCloud, (uint8_t *)data);
                tonie_freshness_check_request__free_unpacked(freshReq, NULL);

                osFreeMem(freshReqCloud.tonie_infos);
                osFreeMem(freshResp.tonie_marked);

                cbr_ctx_t ctx;
                req_cbr_t cbr = getCloudCbr(connection, uri, V1_FRESHNESS_CHECK, &ctx);
                if (!cloud_request_post(NULL, 0, "/v1/freshness-check", data, dataLen, NULL, &cbr))
                {
                    return NO_ERROR;
                }
            }
            else
            {
                tonie_freshness_check_request__free_unpacked(freshReq, NULL);
            }
            setTonieboxSettings(&freshResp);

            size_t dataLen = tonie_freshness_check_response__get_packed_size(&freshResp);
            tonie_freshness_check_response__pack(&freshResp, (uint8_t *)data);
            osFreeMem(freshReqCloud.tonie_infos);
            osFreeMem(freshResp.tonie_marked);
            TRACE_INFO("Freshness check response: size=%zu, content=%s\n", dataLen, data);

            httpPrepareHeader(connection, "application/octet-stream; charset=utf-8", dataLen);
            return httpWriteResponse(connection, data, false);
            // tonie_freshness_check_response__free_unpacked(&freshResp, NULL);
        }
        return NO_ERROR;
    }
    return NO_ERROR;
}