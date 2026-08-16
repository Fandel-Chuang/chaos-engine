/*
 * ChaosEngine etcd 服务发现 - 实现
 *
 * 通过 etcd v3 HTTP/gRPC Gateway REST API 操作。
 * etcd v3 gateway 默认在 2379 端口提供 HTTP API:
 *   PUT  /v3/kv/put        - 写 key (带 lease)
 *   POST /v3/kv/deleterange - 删 key
 *   POST /v3/kv/range       - 读 key (前缀查询)
 *   POST /v3/lease/grant    - 创建 lease
 *   POST /v3/lease/keepalive - 续约 lease
 *   POST /v3/watch          - 监听 key 变更
 *
 * 请求/响应均为 JSON，key/value 为 base64 编码。
 *
 * 纯 C99，libcurl。
 */

#ifdef HAVE_CURL

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE

#include "rpc/ce_etcd_registry.h"
#include "public_api/ce_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

/* ================================================================
 * 内部结构
 * ================================================================ */

/** 本地缓存条目 */
typedef struct CeEtcdCacheEntry {
    char            name[64];
    CeEtcdInstance  instances[CE_ETCD_MAX_INSTANCES];
    int             count;
    int64_t         last_refresh_ms;
    int             rr_index;   /* round-robin 游标 */
    struct CeEtcdCacheEntry* next;
} CeEtcdCacheEntry;

/** 已注册的服务记录（用于心跳和注销） */
typedef struct CeEtcdRegEntry {
    char     name[64];
    char     host[64];
    int      port;
    char     etcd_key[256];     /* etcd 中的完整 key */
    int64_t  lease_id;          /* etcd lease ID */
    struct CeEtcdRegEntry* next;
} CeEtcdRegEntry;

struct CeEtcdClient {
    char       endpoint[256];           /* etcd 地址 */
    CURL*      curl;                    /* libcurl easy handle（复用） */
    pthread_mutex_t mutex;              /* 保护缓存和注册表 */

    /* 已注册服务 */
    CeEtcdRegEntry* reg_list;
    int             reg_count;

    /* 本地缓存 */
    CeEtcdCacheEntry* cache_list;

    /* 心跳线程 */
    pthread_t    heartbeat_thread;
    int          heartbeat_running;
    int          heartbeat_interval_sec;

    /* Watch 线程 */
    pthread_t    watch_thread;
    int          watch_running;
    char         watch_name[64];
    CeEtcdWatchCallback watch_cb;
    void*        watch_user_data;
};

/* ================================================================
 * 工具函数
 * ================================================================ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/** libcurl 写回调 */
struct CurlWriteBuf {
    char*    data;
    size_t   size;
    size_t   cap;
};

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userp) {
    struct CurlWriteBuf* buf = (struct CurlWriteBuf*)userp;
    size_t total = size * nmemb;
    if (buf->size + total + 1 > buf->cap) {
        size_t new_cap = (buf->cap + total) * 2;
        char* new_data = (char*)realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/** 发送 HTTP POST JSON 请求 */
static int etcd_http_post(CeEtcdClient* cli, const char* path,
                            const char* json_body, char** out_resp) {
    char url[512];
    snprintf(url, sizeof(url), "%s%s", cli->endpoint, path);

    struct CurlWriteBuf wbuf = {0};
    wbuf.cap = 4096;
    wbuf.data = (char*)malloc(wbuf.cap);
    if (!wbuf.data) return -1;
    wbuf.data[0] = '\0';

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_reset(cli->curl);
    curl_easy_setopt(cli->curl, CURLOPT_URL, url);
    curl_easy_setopt(cli->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(cli->curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(cli->curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(cli->curl, CURLOPT_WRITEDATA, &wbuf);
    curl_easy_setopt(cli->curl, CURLOPT_TIMEOUT_MS, CE_ETCD_REQUEST_TIMEOUT_MS);

    CURLcode rc = curl_easy_perform(cli->curl);
    curl_slist_free_all(headers);

    long http_code = 0;
    curl_easy_getinfo(cli->curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (rc != CURLE_OK) {
        CE_LOG_ERROR("ETCD", "HTTP POST %s failed: %s", path, curl_easy_strerror(rc));
        free(wbuf.data);
        return -1;
    }

    if (http_code != 200) {
        CE_LOG_ERROR("ETCD", "HTTP POST %s returned %ld: %s", path, http_code, wbuf.data);
        free(wbuf.data);
        return -1;
    }

    if (out_resp) {
        *out_resp = wbuf.data;
    } else {
        free(wbuf.data);
    }
    return 0;
}

/* ---- Base64 编码/解码 ---- */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* base64_encode(const uint8_t* data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = b64_table[(triple >> 6) & 0x3F];
        out[j++] = b64_table[triple & 0x3F];
    }

    size_t pad = (3 - len % 3) % 3;
    for (size_t k = 0; k < pad; k++) out[out_len - 1 - k] = '=';

    out[out_len] = '\0';
    return out;
}

static int base64_decode_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t* base64_decode(const char* data, size_t* out_len) {
    size_t len = strlen(data);
    while (len > 0 && data[len - 1] == '=') len--;
    if (len == 0) {
        *out_len = 0;
        return (uint8_t*)calloc(1, 1);
    }

    uint8_t* out = (uint8_t*)malloc(len * 3 / 4 + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    while (i < len) {
        int a = base64_decode_val(data[i++]);
        int b = (i < len) ? base64_decode_val(data[i++]) : 0;
        int c = (i < len) ? base64_decode_val(data[i++]) : 0;
        int d = (i < len) ? base64_decode_val(data[i++]) : 0;
        if (a < 0 || b < 0) break;
        uint32_t triple = (a << 18) | (b << 12) | ((c >= 0 ? c : 0) << 6) | (d >= 0 ? d : 0);
        out[j++] = (triple >> 16) & 0xFF;
        if (c >= 0) out[j++] = (triple >> 8) & 0xFF;
        if (d >= 0) out[j++] = triple & 0xFF;
    }
    *out_len = j;
    return out;
}

/* ---- JSON 简易解析 ---- */

/** 在 JSON 字符串中提取指定 key 的字符串值 */
static int json_extract_string(const char* json, const char* key, char* out, int out_size) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char* p = strstr(json, search_key);
    if (!p) return -1;
    p += strlen(search_key);
    /* 跳过 : 和空白 */
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

/** 在 JSON 字符串中提取指定 key 的整数值 */
static int json_extract_int(const char* json, const char* key, int* out) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char* p = strstr(json, search_key);
    if (!p) return -1;
    p += strlen(search_key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 0;
}

/** 从 JSON 响应中提取 "ID" 字段（lease ID） */
static int64_t json_extract_int64(const char* json, const char* key) {
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    const char* p = strstr(json, search_key);
    if (!p) return 0;
    p += strlen(search_key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return (int64_t)strtoll(p, NULL, 10);
}

/* ================================================================
 * 客户端生命周期
 * ================================================================ */

CeEtcdClient* ce_etcd_create(const char* endpoint) {
    CeEtcdClient* cli = (CeEtcdClient*)calloc(1, sizeof(*cli));
    if (!cli) return NULL;

    strncpy(cli->endpoint, endpoint ? endpoint : CE_ETCD_DEFAULT_ENDPOINT,
            sizeof(cli->endpoint) - 1);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    cli->curl = curl_easy_init();
    if (!cli->curl) {
        CE_LOG_ERROR("ETCD", "Failed to init curl");
        free(cli);
        return NULL;
    }

    pthread_mutex_init(&cli->mutex, NULL);
    cli->reg_list = NULL;
    cli->reg_count = 0;
    cli->cache_list = NULL;
    cli->heartbeat_running = 0;
    cli->heartbeat_interval_sec = CE_ETCD_HEARTBEAT_INTERVAL;
    cli->watch_running = 0;

    CE_LOG_INFO("ETCD", "Client created: endpoint=%s", cli->endpoint);
    return cli;
}

void ce_etcd_destroy(CeEtcdClient* cli) {
    if (!cli) return;

    /* 停止心跳和 watch 线程 */
    ce_etcd_heartbeat_stop(cli);
    ce_etcd_watch_stop(cli);

    /* 自动注销已注册的服务 */
    pthread_mutex_lock(&cli->mutex);
    CeEtcdRegEntry* reg = cli->reg_list;
    while (reg) {
        /* 删除 etcd key */
        char key_b64[512];
        char* encoded_key = base64_encode((const uint8_t*)reg->etcd_key, strlen(reg->etcd_key));
        if (encoded_key) {
            char body[1024];
            snprintf(body, sizeof(body), "{\"key\":\"%s\"}", encoded_key);
            char* resp = NULL;
            etcd_http_post(cli, "/v3/kv/deleterange", body, &resp);
            free(resp);
            free(encoded_key);
        }
        CeEtcdRegEntry* next = reg->next;
        free(reg);
        reg = next;
    }
    cli->reg_list = NULL;
    cli->reg_count = 0;
    pthread_mutex_unlock(&cli->mutex);

    /* 清理缓存 */
    CeEtcdCacheEntry* cache = cli->cache_list;
    while (cache) {
        CeEtcdCacheEntry* next = cache->next;
        free(cache);
        cache = next;
    }

    if (cli->curl) curl_easy_cleanup(cli->curl);
    curl_global_cleanup();
    pthread_mutex_destroy(&cli->mutex);

    CE_LOG_INFO("ETCD", "Client destroyed");
    free(cli);
}

CeBool ce_etcd_health_check(CeEtcdClient* cli) {
    if (!cli) return CE_FALSE;
    char* resp = NULL;
    int rc = etcd_http_post(cli, "/v3/cluster/member/list", "{}", &resp);
    if (rc == 0) {
        free(resp);
        return CE_TRUE;
    }
    return CE_FALSE;
}

/* ================================================================
 * 服务注册
 * ================================================================ */

/** 构造 etcd key: /chaos/services/{name}/{host}:{port} */
static void build_etcd_key(char* out, int out_size, const char* name, const char* host, int port) {
    snprintf(out, out_size, "%s%s/%s:%d", CE_ETCD_KEY_PREFIX, name, host, port);
}

/** 构造实例 JSON value */
static void build_instance_json(char* out, int out_size, const char* name,
                                  const char* host, int port, int weight,
                                  const char* metadata) {
    snprintf(out, out_size,
             "{\"name\":\"%s\",\"host\":\"%s\",\"port\":%d,\"weight\":%d,\"metadata\":\"%s\"}",
             name, host, port, weight, metadata ? metadata : "");
}

CeResult ce_etcd_register(CeEtcdClient* cli,
                            const char* name,
                            const char* host, int port,
                            int weight,
                            const char* metadata) {
    if (!cli || !name || !host) return CE_ERR;
    if (weight <= 0) weight = 1;

    /* 1. 创建 lease */
    char lease_body[128];
    snprintf(lease_body, sizeof(lease_body),
             "{\"TTL\":%d}", CE_ETCD_DEFAULT_LEASE_TTL);

    char* lease_resp = NULL;
    if (etcd_http_post(cli, "/v3/lease/grant", lease_body, &lease_resp) != 0) {
        CE_LOG_ERROR("ETCD", "Failed to create lease for %s", name);
        return CE_ERR;
    }

    int64_t lease_id = json_extract_int64(lease_resp, "ID");
    free(lease_resp);

    if (lease_id == 0) {
        CE_LOG_ERROR("ETCD", "Invalid lease ID for %s", name);
        return CE_ERR;
    }

    /* 2. PUT key with lease */
    char etcd_key[256];
    build_etcd_key(etcd_key, sizeof(etcd_key), name, host, port);

    char instance_json[512];
    build_instance_json(instance_json, sizeof(instance_json), name, host, port, weight, metadata);

    char* key_b64 = base64_encode((const uint8_t*)etcd_key, strlen(etcd_key));
    char* val_b64 = base64_encode((const uint8_t*)instance_json, strlen(instance_json));

    char put_body[1024];
    snprintf(put_body, sizeof(put_body),
             "{\"key\":\"%s\",\"value\":\"%s\",\"lease\":%lld}",
             key_b64, val_b64, (long long)lease_id);

    char* put_resp = NULL;
    CeResult ret = CE_ERR;
    if (etcd_http_post(cli, "/v3/kv/put", put_body, &put_resp) == 0) {
        ret = CE_OK;
        CE_LOG_INFO("ETCD", "Registered: %s -> %s:%d (lease=%lld)",
                    name, host, port, (long long)lease_id);
    }

    free(key_b64);
    free(val_b64);
    free(put_resp);

    if (ret != CE_OK) return ret;

    /* 3. 记录注册信息（用于心跳和注销） */
    pthread_mutex_lock(&cli->mutex);
    CeEtcdRegEntry* reg = (CeEtcdRegEntry*)calloc(1, sizeof(*reg));
    if (reg) {
        strncpy(reg->name, name, sizeof(reg->name) - 1);
        strncpy(reg->host, host, sizeof(reg->host) - 1);
        reg->port = port;
        strncpy(reg->etcd_key, etcd_key, sizeof(reg->etcd_key) - 1);
        reg->lease_id = lease_id;
        reg->next = cli->reg_list;
        cli->reg_list = reg;
        cli->reg_count++;
    }
    pthread_mutex_unlock(&cli->mutex);

    return CE_OK;
}

CeResult ce_etcd_deregister(CeEtcdClient* cli,
                              const char* name,
                              const char* host, int port) {
    if (!cli || !name || !host) return CE_ERR;

    /* 从注册表查找 */
    pthread_mutex_lock(&cli->mutex);
    CeEtcdRegEntry** pp = &cli->reg_list;
    CeEtcdRegEntry* found = NULL;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0 &&
            strcmp((*pp)->host, host) == 0 &&
            (*pp)->port == port) {
            found = *pp;
            *pp = found->next;
            cli->reg_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&cli->mutex);

    if (!found) {
        CE_LOG_WARN("ETCD", "deregister: %s not found in reg list", name);
        return CE_ERR;
    }

    /* 删除 etcd key */
    char* key_b64 = base64_encode((const uint8_t*)found->etcd_key, strlen(found->etcd_key));
    if (key_b64) {
        char body[1024];
        snprintf(body, sizeof(body), "{\"key\":\"%s\"}", key_b64);
        char* resp = NULL;
        etcd_http_post(cli, "/v3/kv/deleterange", body, &resp);
        free(resp);
        free(key_b64);
    }

    CE_LOG_INFO("ETCD", "Deregistered: %s -> %s:%d", name, host, port);
    free(found);
    return CE_OK;
}

CeResult ce_etcd_heartbeat(CeEtcdClient* cli) {
    if (!cli) return CE_ERR;

    pthread_mutex_lock(&cli->mutex);
    CeEtcdRegEntry* reg = cli->reg_list;
    int fail_count = 0;
    while (reg) {
        char body[256];
        snprintf(body, sizeof(body), "{\"ID\":%lld}", (long long)reg->lease_id);
        char* resp = NULL;
        if (etcd_http_post(cli, "/v3/lease/keepalive", body, &resp) != 0) {
            fail_count++;
            CE_LOG_WARN("ETCD", "Heartbeat failed for %s (lease=%lld)",
                        reg->name, (long long)reg->lease_id);
        }
        free(resp);
        reg = reg->next;
    }
    pthread_mutex_unlock(&cli->mutex);

    return (fail_count == 0) ? CE_OK : CE_ERR;
}

/* ---- 心跳线程 ---- */

static void* heartbeat_thread_fn(void* arg) {
    CeEtcdClient* cli = (CeEtcdClient*)arg;
    while (cli->heartbeat_running) {
        ce_etcd_heartbeat(cli);
        for (int i = 0; i < cli->heartbeat_interval_sec * 10 && cli->heartbeat_running; i++) {
            usleep(100000);  /* 100ms, 可中断 */
        }
    }
    return NULL;
}

CeResult ce_etcd_heartbeat_start(CeEtcdClient* cli) {
    if (!cli || cli->heartbeat_running) return CE_ERR;
    cli->heartbeat_running = 1;
    if (pthread_create(&cli->heartbeat_thread, NULL, heartbeat_thread_fn, cli) != 0) {
        cli->heartbeat_running = 0;
        CE_LOG_ERROR("ETCD", "Failed to start heartbeat thread");
        return CE_ERR;
    }
    CE_LOG_INFO("ETCD", "Heartbeat thread started (interval=%ds)", cli->heartbeat_interval_sec);
    return CE_OK;
}

void ce_etcd_heartbeat_stop(CeEtcdClient* cli) {
    if (!cli || !cli->heartbeat_running) return;
    cli->heartbeat_running = 0;
    pthread_join(cli->heartbeat_thread, NULL);
    CE_LOG_INFO("ETCD", "Heartbeat thread stopped");
}

/* ================================================================
 * 服务发现
 * ================================================================ */

/** 从 etcd 响应中解析实例列表 */
static int parse_instances_from_response(const char* resp, CeEtcdInstance* out_arr, int max_count) {
    int count = 0;
    const char* p = resp;

    while (count < max_count) {
        /* 查找 "value" 字段（base64 编码的 JSON） */
        const char* value_pos = strstr(p, "\"value\"");
        if (!value_pos) break;

        value_pos += 7;  /* skip "value" */
        while (*value_pos == ':' || *value_pos == ' ' || *value_pos == '\t') value_pos++;
        if (*value_pos != '"') { p = value_pos; continue; }
        value_pos++;

        /* 提取 base64 字符串 */
        char b64_buf[1024];
        int i = 0;
        while (*value_pos && *value_pos != '"' && i < (int)sizeof(b64_buf) - 1) {
            b64_buf[i++] = *value_pos++;
        }
        b64_buf[i] = '\0';

        /* base64 解码 */
        size_t decoded_len = 0;
        uint8_t* decoded = base64_decode(b64_buf, &decoded_len);
        if (!decoded || decoded_len == 0) {
            free(decoded);
            p = value_pos;
            continue;
        }

        /* 从 JSON 中解析实例信息 */
        char* json_str = (char*)decoded;
        json_str[decoded_len] = '\0';  /* 确保以 null 结尾 */

        CeEtcdInstance* inst = &out_arr[count];
        memset(inst, 0, sizeof(*inst));
        json_extract_string(json_str, "host", inst->host, sizeof(inst->host));
        json_extract_int(json_str, "port", &inst->port);
        json_extract_int(json_str, "weight", &inst->weight);
        json_extract_string(json_str, "name", inst->name, sizeof(inst->name));
        json_extract_string(json_str, "metadata", inst->metadata, sizeof(inst->metadata));
        inst->registered_ms = now_ms();

        free(decoded);
        count++;
        p = value_pos;
    }

    return count;
}

/** 从 etcd 拉取服务实例列表 */
static int fetch_instances(CeEtcdClient* cli, const char* name,
                             CeEtcdInstance* out_arr, int max_count) {
    /* 构造前缀 key */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s%s/", CE_ETCD_KEY_PREFIX, name);
    char* prefix_b64 = base64_encode((const uint8_t*)prefix, strlen(prefix));

    char body[512];
    snprintf(body, sizeof(body),
             "{\"key\":\"%s\",\"range_end\":\"%s\",\"limit\":%d}",
             prefix_b64, prefix_b64, max_count);
    /* range_end 用前缀的下一个字符替换最后一个字符 */
    /* 简化: 直接用 prefix + 1 */
    int prefix_len = (int)strlen(prefix);
    char range_end[256];
    strncpy(range_end, prefix, sizeof(range_end) - 1);
    range_end[sizeof(range_end) - 1] = '\0';
    range_end[prefix_len - 1]++;  /* / -> 0, 即前缀范围 */
    char* range_end_b64 = base64_encode((const uint8_t*)range_end, strlen(range_end));

    snprintf(body, sizeof(body),
             "{\"key\":\"%s\",\"range_end\":\"%s\",\"limit\":%d}",
             prefix_b64, range_end_b64, max_count);

    free(prefix_b64);
    free(range_end_b64);

    char* resp = NULL;
    if (etcd_http_post(cli, "/v3/kv/range", body, &resp) != 0) {
        return -1;
    }

    int count = parse_instances_from_response(resp, out_arr, max_count);
    free(resp);
    return count;
}

/** 查找或创建缓存条目 */
static CeEtcdCacheEntry* cache_find_or_create(CeEtcdClient* cli, const char* name) {
    CeEtcdCacheEntry* e = cli->cache_list;
    while (e) {
        if (strcmp(e->name, name) == 0) return e;
        e = e->next;
    }
    e = (CeEtcdCacheEntry*)calloc(1, sizeof(*e));
    if (!e) return NULL;
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->next = cli->cache_list;
    cli->cache_list = e;
    return e;
}

CeResult ce_etcd_discover(CeEtcdClient* cli,
                            const char* name,
                            CeEtcdLbStrategy strategy,
                            CeEtcdInstance* out_inst) {
    if (!cli || !name || !out_inst) return CE_ERR;

    pthread_mutex_lock(&cli->mutex);

    CeEtcdCacheEntry* cache = cache_find_or_create(cli, name);
    if (!cache) {
        pthread_mutex_unlock(&cli->mutex);
        return CE_ERR;
    }

    /* 检查缓存是否过期 */
    int64_t now = now_ms();
    if (cache->count == 0 || (now - cache->last_refresh_ms) > (CE_ETCD_CACHE_TTL_SEC * 1000)) {
        /* 缓存过期，从 etcd 拉取 */
        CeEtcdInstance instances[CE_ETCD_MAX_INSTANCES];
        int count = fetch_instances(cli, name, instances, CE_ETCD_MAX_INSTANCES);
        if (count > 0) {
            memcpy(cache->instances, instances, sizeof(CeEtcdInstance) * count);
            cache->count = count;
            cache->last_refresh_ms = now;
        } else if (cache->count == 0) {
            /* 首次拉取失败且无缓存 */
            pthread_mutex_unlock(&cli->mutex);
            CE_LOG_WARN("ETCD", "discover: no instances for %s", name);
            return CE_ERR;
        }
        /* 拉取失败但有缓存，继续用缓存 */
    }

    if (cache->count == 0) {
        pthread_mutex_unlock(&cli->mutex);
        return CE_ERR;
    }

    /* 负载均衡选择 */
    int selected = 0;
    switch (strategy) {
    case CE_ETCD_LB_ROUND_ROBIN:
        selected = cache->rr_index % cache->count;
        cache->rr_index = (cache->rr_index + 1) % cache->count;
        break;

    case CE_ETCD_LB_RANDOM:
        srand((unsigned)now);
        selected = rand() % cache->count;
        break;

    case CE_ETCD_LB_LEAST_CONN:
        /* 简化: 选权重最高的 */
        {
            int best_weight = -1;
            for (int i = 0; i < cache->count; i++) {
                if (cache->instances[i].weight > best_weight) {
                    best_weight = cache->instances[i].weight;
                    selected = i;
                }
            }
        }
        break;
    }

    *out_inst = cache->instances[selected];
    pthread_mutex_unlock(&cli->mutex);

    CE_LOG_DEBUG("ETCD", "discover: %s -> %s:%d (strategy=%s, index=%d/%d)",
                 name, out_inst->host, out_inst->port,
                 ce_etcd_lb_name(strategy), selected, cache->count);
    return CE_OK;
}

CeResult ce_etcd_list(CeEtcdClient* cli,
                        const char* name,
                        CeEtcdInstance* out_arr,
                        int* out_count) {
    if (!cli || !name || !out_arr || !out_count) return CE_ERR;

    int count = fetch_instances(cli, name, out_arr, CE_ETCD_MAX_INSTANCES);
    if (count < 0) return CE_ERR;

    *out_count = count;
    return CE_OK;
}

CeResult ce_etcd_refresh_cache(CeEtcdClient* cli, const char* name) {
    if (!cli || !name) return CE_ERR;

    pthread_mutex_lock(&cli->mutex);
    CeEtcdCacheEntry* cache = cache_find_or_create(cli, name);
    if (!cache) {
        pthread_mutex_unlock(&cli->mutex);
        return CE_ERR;
    }

    CeEtcdInstance instances[CE_ETCD_MAX_INSTANCES];
    int count = fetch_instances(cli, name, instances, CE_ETCD_MAX_INSTANCES);
    if (count > 0) {
        memcpy(cache->instances, instances, sizeof(CeEtcdInstance) * count);
        cache->count = count;
        cache->last_refresh_ms = now_ms();
    }
    pthread_mutex_unlock(&cli->mutex);
    return (count >= 0) ? CE_OK : CE_ERR;
}

/* ================================================================
 * Watch 机制
 * ================================================================ */

static void* watch_thread_fn(void* arg) {
    CeEtcdClient* cli = (CeEtcdClient*)arg;

    /* 构造 watch 前缀 */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s%s/", CE_ETCD_KEY_PREFIX, cli->watch_name);
    char* prefix_b64 = base64_encode((const uint8_t*)prefix, strlen(prefix));

    char range_end[256];
    strncpy(range_end, prefix, sizeof(range_end) - 1);
    range_end[strlen(prefix) - 1]++;
    char* range_end_b64 = base64_encode((const uint8_t*)range_end, strlen(range_end));

    /* etcd v3 watch 是一个长连接 POST，会持续返回事件 */
    /* 简化实现: 定期轮询 range 查询，对比变化 */
    CeEtcdInstance prev_instances[CE_ETCD_MAX_INSTANCES];
    int prev_count = 0;

    while (cli->watch_running) {
        CeEtcdInstance curr[CE_ETCD_MAX_INSTANCES];
        int curr_count = fetch_instances(cli, cli->watch_name, curr, CE_ETCD_MAX_INSTANCES);

        if (curr_count >= 0 && curr_count != prev_count) {
            /* 数量变化，通知回调 */
            for (int i = 0; i < curr_count && cli->watch_cb; i++) {
                /* 检查是否是新增 */
                CeBool is_new = CE_TRUE;
                for (int j = 0; j < prev_count; j++) {
                    if (strcmp(curr[i].host, prev_instances[j].host) == 0 &&
                        curr[i].port == prev_instances[j].port) {
                        is_new = CE_FALSE;
                        break;
                    }
                }
                if (is_new) {
                    cli->watch_cb(CE_ETCD_EVENT_PUT, &curr[i], cli->watch_user_data);
                }
            }
            for (int j = 0; j < prev_count && cli->watch_cb; j++) {
                CeBool still_exists = CE_FALSE;
                for (int i = 0; i < curr_count; i++) {
                    if (strcmp(prev_instances[j].host, curr[i].host) == 0 &&
                        prev_instances[j].port == curr[i].port) {
                        still_exists = CE_TRUE;
                        break;
                    }
                }
                if (!still_exists) {
                    cli->watch_cb(CE_ETCD_EVENT_DELETE, &prev_instances[j], cli->watch_user_data);
                }
            }

            /* 更新 prev */
            memcpy(prev_instances, curr, sizeof(CeEtcdInstance) * curr_count);
            prev_count = curr_count;
        }

        /* 等待下一轮（可中断） */
        for (int i = 0; i < CE_ETCD_HEARTBEAT_INTERVAL * 10 && cli->watch_running; i++) {
            usleep(100000);
        }
    }

    free(prefix_b64);
    free(range_end_b64);
    return NULL;
}

CeResult ce_etcd_watch(CeEtcdClient* cli,
                         const char* name,
                         CeEtcdWatchCallback callback,
                         void* user_data) {
    if (!cli || !name || !callback) return CE_ERR;
    if (cli->watch_running) return CE_ERR;

    strncpy(cli->watch_name, name, sizeof(cli->watch_name) - 1);
    cli->watch_cb = callback;
    cli->watch_user_data = user_data;
    cli->watch_running = 1;

    if (pthread_create(&cli->watch_thread, NULL, watch_thread_fn, cli) != 0) {
        cli->watch_running = 0;
        return CE_ERR;
    }

    CE_LOG_INFO("ETCD", "Watch started for: %s", name);
    return CE_OK;
}

void ce_etcd_watch_stop(CeEtcdClient* cli) {
    if (!cli || !cli->watch_running) return;
    cli->watch_running = 0;
    pthread_join(cli->watch_thread, NULL);
    CE_LOG_INFO("ETCD", "Watch stopped");
}

/* ================================================================
 * 工具函数
 * ================================================================ */

const char* ce_etcd_lb_name(CeEtcdLbStrategy strategy) {
    switch (strategy) {
    case CE_ETCD_LB_ROUND_ROBIN: return "round_robin";
    case CE_ETCD_LB_RANDOM:      return "random";
    case CE_ETCD_LB_LEAST_CONN:  return "least_conn";
    default: return "unknown";
    }
}

#else  /* !HAVE_CURL */

/* 无 libcurl 时提供存根实现，返回错误 */

#include "rpc/ce_etcd_registry.h"
#include <stdlib.h>

CeEtcdClient* ce_etcd_create(const char* endpoint) { (void)endpoint; return NULL; }
void ce_etcd_destroy(CeEtcdClient* cli) { (void)cli; }
CeBool ce_etcd_health_check(CeEtcdClient* cli) { (void)cli; return CE_FALSE; }
CeResult ce_etcd_register(CeEtcdClient* cli, const char* name, const char* host, int port, int weight, const char* metadata) {
    (void)cli; (void)name; (void)host; (void)port; (void)weight; (void)metadata; return CE_ERR;
}
CeResult ce_etcd_deregister(CeEtcdClient* cli, const char* name, const char* host, int port) {
    (void)cli; (void)name; (void)host; (void)port; return CE_ERR;
}
CeResult ce_etcd_heartbeat(CeEtcdClient* cli) { (void)cli; return CE_ERR; }
CeResult ce_etcd_heartbeat_start(CeEtcdClient* cli) { (void)cli; return CE_ERR; }
void ce_etcd_heartbeat_stop(CeEtcdClient* cli) { (void)cli; }
CeResult ce_etcd_discover(CeEtcdClient* cli, const char* name, CeEtcdLbStrategy strategy, CeEtcdInstance* out_inst) {
    (void)cli; (void)name; (void)strategy; (void)out_inst; return CE_ERR;
}
CeResult ce_etcd_list(CeEtcdClient* cli, const char* name, CeEtcdInstance* out_arr, int* out_count) {
    (void)cli; (void)name; (void)out_arr; (void)out_count; return CE_ERR;
}
CeResult ce_etcd_refresh_cache(CeEtcdClient* cli, const char* name) { (void)cli; (void)name; return CE_ERR; }
CeResult ce_etcd_watch(CeEtcdClient* cli, const char* name, CeEtcdWatchCallback callback, void* user_data) {
    (void)cli; (void)name; (void)callback; (void)user_data; return CE_ERR;
}
void ce_etcd_watch_stop(CeEtcdClient* cli) { (void)cli; }
const char* ce_etcd_lb_name(CeEtcdLbStrategy strategy) {
    switch (strategy) {
    case CE_ETCD_LB_ROUND_ROBIN: return "round_robin";
    case CE_ETCD_LB_RANDOM:      return "random";
    case CE_ETCD_LB_LEAST_CONN:  return "least_conn";
    default: return "unknown";
    }
}

#endif /* HAVE_CURL */
