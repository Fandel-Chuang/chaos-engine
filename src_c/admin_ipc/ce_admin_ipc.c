/*
 * ChaosEngine Admin IPC — Unix Domain Socket JSON-RPC 2.0 Server
 * 纯 C99，单连接模式，独立线程
 *
 * 协议：换行分隔的 JSON 行（每行一条完整的 JSON-RPC 请求/响应）
 * 支持方法：stats, aoi, cell, network, memory, log, render, system, health
 */

#include "admin_ipc/ce_admin_ipc.h"

#include "public_api/chaos_engine.h"
#include "public_api/ce_ecs.h"
#include "public_api/ce_log.h"

#include "server/ce_aoi.h"
#include "server/ce_cell.h"
#include "server/ce_server_types.h"

#include "network/ce_async_io.h"
#include "network/ce_ebpf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ---- 常量 ---- */

#define DEFAULT_SOCKET_PATH      "/tmp/chaos_admin.sock"
#define MAX_REQUEST_LEN          65536
#define MAX_RESPONSE_LEN         262144
#define RECV_BUFFER_SIZE         65536
#define MAX_LOG_LINES_DEFAULT    50
#define MAX_TOP_ENTITIES         10
#define MAX_CUSTOM_HANDLERS      8

/* ---- 全局自定义处理器（供 process_request 访问） ---- */

static CeAdminIpcHandler   g_custom_handlers[MAX_CUSTOM_HANDLERS];
static void*               g_custom_handler_data[MAX_CUSTOM_HANDLERS];
static int                 g_custom_handler_count = 0;
static pthread_mutex_t     g_handler_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- 内部结构 ---- */

struct CeAdminIpc {
    char            socket_path[256];
    int             listen_fd;
    int             client_fd;
    CeBool          running;
    pthread_t       thread;
    pthread_mutex_t mutex;
};

/* ---- 前向声明 ---- */

static void* ipc_thread_func(void* arg);
static int   create_listen_socket(const char* path);
static void  handle_client(int client_fd);
static int   recv_line(int fd, char* buf, int max_len);
static int   send_response(int fd, const char* json);
static int   process_request(const char* request_json, char* response_buf, int max_len);

/* ---- JSON 辅助函数 ---- */

/* 写入 JSON 字符串值（带转义） */
static int json_append_string(char* buf, int offset, int max_len, const char* str) {
    int n = 0;
    n = snprintf(buf + offset, max_len - offset, "\"");
    if (n < 0) return offset;
    offset += n;
    if (offset >= max_len) return offset;

    if (str) {
        const char* p = str;
        while (*p && offset < max_len - 2) {
            switch (*p) {
            case '"':  buf[offset++] = '\\'; buf[offset++] = '"';  break;
            case '\\': buf[offset++] = '\\'; buf[offset++] = '\\'; break;
            case '\n': buf[offset++] = '\\'; buf[offset++] = 'n';  break;
            case '\r': buf[offset++] = '\\'; buf[offset++] = 'r';  break;
            case '\t': buf[offset++] = '\\'; buf[offset++] = 't';  break;
            default:   buf[offset++] = *p; break;
            }
            p++;
        }
    }

    n = snprintf(buf + offset, max_len - offset, "\"");
    if (n < 0) return offset;
    offset += n;
    return offset;
}

/* 写入 JSON 整数值 */
static int json_append_int(char* buf, int offset, int max_len, int64_t val) {
    int n = snprintf(buf + offset, max_len - offset, "%ld", (long)val);
    if (n < 0) return offset;
    return offset + n;
}

/* 写入 JSON 浮点值 */
static int json_append_double(char* buf, int offset, int max_len, double val) {
    int n = snprintf(buf + offset, max_len - offset, "%.6f", val);
    if (n < 0) return offset;
    return offset + n;
}

/* 写入 JSON 布尔值 */
static int json_append_bool(char* buf, int offset, int max_len, CeBool val) {
    const char* s = val ? "true" : "false";
    int n = snprintf(buf + offset, max_len - offset, "%s", s);
    if (n < 0) return offset;
    return offset + n;
}

/* 写入 JSON 键值对：字符串 */
static int json_append_kv_string(char* buf, int offset, int max_len,
                                  const char* key, const char* val) {
    offset = json_append_string(buf, offset, max_len, key);
    if (offset >= max_len) return offset;
    int n = snprintf(buf + offset, max_len - offset, ":");
    if (n < 0) return offset;
    offset += n;
    offset = json_append_string(buf, offset, max_len, val);
    return offset;
}

/* 写入 JSON 键值对：整数 */
static int json_append_kv_int(char* buf, int offset, int max_len,
                               const char* key, int64_t val) {
    offset = json_append_string(buf, offset, max_len, key);
    if (offset >= max_len) return offset;
    int n = snprintf(buf + offset, max_len - offset, ":");
    if (n < 0) return offset;
    offset += n;
    offset = json_append_int(buf, offset, max_len, val);
    return offset;
}

/* 写入 JSON 键值对：浮点 */
static int json_append_kv_double(char* buf, int offset, int max_len,
                                  const char* key, double val) {
    offset = json_append_string(buf, offset, max_len, key);
    if (offset >= max_len) return offset;
    int n = snprintf(buf + offset, max_len - offset, ":");
    if (n < 0) return offset;
    offset += n;
    offset = json_append_double(buf, offset, max_len, val);
    return offset;
}

/* 写入 JSON 键值对：布尔 */
static int json_append_kv_bool(char* buf, int offset, int max_len,
                                const char* key, CeBool val) {
    offset = json_append_string(buf, offset, max_len, key);
    if (offset >= max_len) return offset;
    int n = snprintf(buf + offset, max_len - offset, ":");
    if (n < 0) return offset;
    offset += n;
    offset = json_append_bool(buf, offset, max_len, val);
    return offset;
}

/* 提取 JSON 字符串中指定 key 的字符串值（简易解析，不依赖第三方库） */
static int json_extract_string(const char* json, const char* key, char* out, int max_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    /* 跳过 : 和空白 */
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return -1;
    pos++; /* 跳过开头引号 */
    int i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++;
            switch (*pos) {
            case '"':  out[i++] = '"';  break;
            case '\\': out[i++] = '\\'; break;
            case 'n':  out[i++] = '\n'; break;
            case 'r':  out[i++] = '\r'; break;
            case 't':  out[i++] = '\t'; break;
            default:   out[i++] = *pos; break;
            }
        } else {
            out[i++] = *pos;
        }
        pos++;
    }
    out[i] = '\0';
    return i;
}

/* 提取 JSON 中指定 key 的整数值 */
static int json_extract_int(const char* json, const char* key, int64_t* out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') return -1; /* 字符串值，不是整数 */
    char* end = NULL;
    long val = strtol(pos, &end, 10);
    if (end == pos) return -1;
    *out = (int64_t)val;
    return 0;
}

/* 构造 JSON-RPC 2.0 成功响应头 */
static int json_rpc_result_start(char* buf, int max_len, const char* id_str) {
    int offset = 0;
    int n = snprintf(buf + offset, max_len - offset,
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{", id_str);
    if (n < 0) return 0;
    offset += n;
    return offset;
}

/* 构造 JSON-RPC 2.0 错误响应 */
static int json_rpc_error(char* buf, int max_len, const char* id_str,
                           int code, const char* message) {
    return snprintf(buf, max_len,
                    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                    id_str, code, message);
}

/* ---- 方法处理器 ---- */

static int handle_stats(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    uint32_t entity_count    = ce_ecs_get_entity_count();
    uint32_t component_count = ce_ecs_get_component_count();
    double   total_time      = ce_time_get_total();
    double   delta_time      = ce_time_get_delta();
    double   fps             = (delta_time > 0.0) ? (1.0 / delta_time) : 0.0;
    double   frame_time_us   = delta_time * 1000000.0;
    double   uptime          = total_time;

    offset = json_append_kv_int(buf, offset, max_len, "entity_count", entity_count);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "component_count", component_count);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "fps", fps);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "frame_time_us", frame_time_us);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "uptime", uptime);
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_aoi(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    int aoi_entity_count = ce_aoi_entity_count();
    /* AOI radius is internal; use a reasonable default if not exposed */
    float aoi_radius = 100.0f;

    offset = json_append_kv_int(buf, offset, max_len, "entity_count", aoi_entity_count);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "aoi_radius", aoi_radius);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    /* events: enter/leave/move counts (simulated) */
    n = snprintf(buf + offset, max_len - offset,
                 "\"events\":{\"enter\":0,\"leave\":0,\"move\":0},");
    if (n < 0) return -1;
    offset += n;

    /* top_entities: 前 MAX_TOP_ENTITIES 个实体 ID */
    n = snprintf(buf + offset, max_len - offset, "\"top_entities\":[");
    if (n < 0) return -1;
    offset += n;

    int top_count = aoi_entity_count;
    if (top_count > MAX_TOP_ENTITIES) top_count = MAX_TOP_ENTITIES;
    for (int i = 0; i < top_count; i++) {
        if (i > 0) {
            n = snprintf(buf + offset, max_len - offset, ",");
            if (n < 0) return -1;
            offset += n;
        }
        offset = json_append_int(buf, offset, max_len, i);
        if (offset >= max_len) return -1;
    }
    n = snprintf(buf + offset, max_len - offset, "]");
    if (n < 0) return -1;
    offset += n;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_cell(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    int total_cells = ce_cell_count();
    int active_cells = 0;
    float cell_size = 100.0f;
    float world_size = 1000.0f;

    /* 统计活跃 Cell */
    for (int i = 0; i < total_cells; i++) {
        const CeCell* cell = ce_cell_get((CeCellId)i);
        if (cell && cell->state == CE_CELL_ACTIVE) {
            active_cells++;
            /* 尝试从第一个活跃 Cell 获取尺寸 */
            if (active_cells == 1) {
                cell_size = cell->bounds.max_x - cell->bounds.min_x;
                world_size = cell->bounds.max_x > world_size ? cell->bounds.max_x : world_size;
                world_size = cell->bounds.max_y > world_size ? cell->bounds.max_y : world_size;
            }
        }
    }

    offset = json_append_kv_string(buf, offset, max_len, "grid",
                                    total_cells > 0 ? "uniform" : "none");
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "cell_size", cell_size);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "world_size", world_size);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "total_cells", total_cells);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "active_cells", active_cells);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    /* cells 列表 */
    n = snprintf(buf + offset, max_len - offset, "\"cells\":[");
    if (n < 0) return -1;
    offset += n;

    int cell_count = 0;
    for (int i = 0; i < total_cells && cell_count < 20; i++) {
        const CeCell* cell = ce_cell_get((CeCellId)i);
        if (!cell || cell->id == CE_INVALID_CELL_ID) continue;

        if (cell_count > 0) {
            n = snprintf(buf + offset, max_len - offset, ",");
            if (n < 0) return -1;
            offset += n;
        }

        n = snprintf(buf + offset, max_len - offset,
                     "{\"id\":%u,\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,"
                     "\"entity_count\":%d,\"state\":%d,\"process_id\":%d}",
                     cell->id,
                     cell->bounds.min_x, cell->bounds.min_y,
                     cell->bounds.max_x - cell->bounds.min_x,
                     cell->bounds.max_y - cell->bounds.min_y,
                     cell->entity_count, (int)cell->state, cell->process_id);
        if (n < 0) return -1;
        offset += n;
        cell_count++;
    }
    n = snprintf(buf + offset, max_len - offset, "]");
    if (n < 0) return -1;
    offset += n;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

/* 从 /proc/net/dev 读取所有接口的累计流量 */
static void read_proc_net_dev(int64_t* bytes_in, int64_t* bytes_out) {
    *bytes_in = 0;
    *bytes_out = 0;
    FILE* f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[512];
    /* 跳过前两行（标题） */
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        /* 格式: 接口名: rx_bytes rx_packets ... tx_bytes tx_packets ... */
        char* p = strchr(line, ':');
        if (!p) continue;
        p++;
        /* 跳过接口名后的空白 */
        while (*p == ' ' || *p == '\t') p++;
        /* 第一个字段是 rx_bytes */
        int64_t rx = strtoll(p, &p, 10);
        /* 跳过 7 个字段到达 tx_bytes */
        for (int i = 0; i < 7; i++) {
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        int64_t tx = strtoll(p, NULL, 10);
        *bytes_in += rx;
        *bytes_out += tx;
    }
    fclose(f);
}

static int handle_network(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    const char* backend = ce_async_backend_name();
    CeBool zcrx = ce_async_has_zcrx();

    /* 从 /proc/net/dev 读取真实网络流量 */
    int64_t bytes_in = 0, bytes_out = 0;
    read_proc_net_dev(&bytes_in, &bytes_out);

    /* 计算网速（bytes/sec）和峰值 */
    static int64_t  last_bytes_in  = 0;
    static int64_t  last_bytes_out = 0;
    static double   last_time      = 0.0;
    static double   peak_in_rate   = 0.0;
    static double   peak_out_rate  = 0.0;

    double now_sec = ce_time_get_total();
    double rate_in  = 0.0;
    double rate_out = 0.0;

    if (last_time > 0.0 && now_sec > last_time) {
        double dt = now_sec - last_time;
        rate_in  = (double)(bytes_in  - last_bytes_in)  / dt;
        rate_out = (double)(bytes_out - last_bytes_out) / dt;
        if (rate_in  > peak_in_rate)  peak_in_rate  = rate_in;
        if (rate_out > peak_out_rate) peak_out_rate = rate_out;
    }

    last_bytes_in  = bytes_in;
    last_bytes_out = bytes_out;
    last_time      = now_sec;

    /* 从 /proc/net/tcp 统计 7777 端口活跃连接数 */
    int connections = 0;
    {
        FILE* ftcp = fopen("/proc/net/tcp", "r");
        if (ftcp) {
            char line[256];
            fgets(line, sizeof(line), ftcp); /* 跳过标题 */
            while (fgets(line, sizeof(line), ftcp)) {
                /* 格式: sl local_address rem_address st ... */
                /* local_address: hex_ip:hex_port, st: 01=ESTABLISHED */
                char local_addr[64], rem_addr[64];
                int state;
                if (sscanf(line, "%*d: %63s %63s %x", local_addr, rem_addr, &state) >= 3) {
                    /* 7777 = 0x1E61, 检查本地端口 */
                    if (state == 1 && strstr(local_addr, ":1E61")) { /* 01 = TCP_ESTABLISHED */
                        connections++;
                    }
                }
            }
            fclose(ftcp);
        }
    }

    offset = json_append_kv_int(buf, offset, max_len, "connections", connections);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "bytes_in", bytes_in);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "bytes_out", bytes_out);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "rate_in", rate_in);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "rate_out", rate_out);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "peak_in_rate", peak_in_rate);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "peak_out_rate", peak_out_rate);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "retransmits", 0);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "backend", backend ? backend : "unknown");
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_bool(buf, offset, max_len, "zcrx", zcrx);
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

/* 从 /proc/self/status 读取指定 key 的值（单位 kB） */
static int64_t read_proc_status_kb(const char* key) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    int64_t val = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            const char* p = line + strlen(key);
            while (*p == ':' || *p == ' ' || *p == '\t') p++;
            val = strtoll(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}

static int handle_memory(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    /* 从 /proc/self/status 读取真实内存数据 */
    int64_t vm_rss  = read_proc_status_kb("VmRSS");   /* 实际物理内存 (kB) */
    int64_t vm_peak = read_proc_status_kb("VmPeak");  /* 峰值 (kB) */
    int64_t vm_size = read_proc_status_kb("VmSize");  /* 虚拟内存 (kB) */
    int64_t vm_data = read_proc_status_kb("VmData");  /* 堆内存 (kB) */
    int64_t vm_stk  = read_proc_status_kb("VmStk");   /* 栈内存 (kB) */

    /* 转换为字节 */
    int64_t used   = vm_rss  * 1024;
    int64_t peak   = vm_peak * 1024;
    int64_t vmsize = vm_size * 1024;
    int64_t heap   = vm_data * 1024;
    int64_t stack  = vm_stk  * 1024;

    offset = json_append_kv_int(buf, offset, max_len, "used", used);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "peak", peak);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "virtual", vmsize);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "heap", heap);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "stack", stack);
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

/* ---- CPU 统计：机器级 + 进程级 ---- */

/* 读取 /proc/stat 第一行 (cpu total) */
static int read_proc_stat_cpu(int64_t* user, int64_t* nice, int64_t* system,
                               int64_t* idle, int64_t* iowait,
                               int64_t* irq, int64_t* softirq, int64_t* steal) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    /* 格式: cpu  user nice system idle iowait irq softirq steal ... */
    return sscanf(line, "cpu  %ld %ld %ld %ld %ld %ld %ld %ld",
                  user, nice, system, idle, iowait, irq, softirq, steal);
}

/* 使用 getrusage 获取整个进程的 CPU 时间（非线程级） */

static int handle_cpu(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    /* ---- 机器级 CPU ---- */
    int64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (read_proc_stat_cpu(&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8) {
        int64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
        int64_t used  = total - idle - iowait;
        double cpu_pct = (total > 0) ? ((double)used / (double)total) * 100.0 : 0.0;

        offset = json_append_kv_double(buf, offset, max_len, "machine_pct", cpu_pct);
        if (offset >= max_len) return -1;
        int n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_int(buf, offset, max_len, "machine_user", user);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_int(buf, offset, max_len, "machine_system", system);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_int(buf, offset, max_len, "machine_idle", idle);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_int(buf, offset, max_len, "machine_iowait", iowait);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;
    } else {
        offset = json_append_kv_double(buf, offset, max_len, "machine_pct", 0.0);
        if (offset >= max_len) return -1;
        int n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;
    }

    /* ---- 进程级 CPU（getrusage，全进程而非单线程） ---- */
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        double utime_sec = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1000000.0;
        double stime_sec = (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1000000.0;
        double proc_total = utime_sec + stime_sec;

        long ncores = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncores <= 0) ncores = 1;

        /* 进程 CPU 百分比（两次调用之间的 delta） */
        static double last_proc_total = 0.0;
        static double last_wall_time  = 0.0;
        double now_wall = ce_time_get_total();
        double proc_pct = 0.0;
        if (last_wall_time > 0.0 && now_wall > last_wall_time) {
            double cpu_delta = proc_total - last_proc_total;
            double wall_delta = now_wall - last_wall_time;
            proc_pct = (cpu_delta / wall_delta / (double)ncores) * 100.0;
            if (proc_pct < 0.0) proc_pct = 0.0;
            if (proc_pct > 100.0) proc_pct = 100.0;
        }
        last_proc_total = proc_total;
        last_wall_time  = now_wall;

        offset = json_append_kv_double(buf, offset, max_len, "process_pct", proc_pct);
        if (offset >= max_len) return -1;
        int n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_double(buf, offset, max_len, "process_user_sec", utime_sec);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_double(buf, offset, max_len, "process_sys_sec", stime_sec);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_double(buf, offset, max_len, "process_cpu_seconds", proc_total);
        if (offset >= max_len) return -1;
        n = snprintf(buf + offset, max_len - offset, ",");
        if (n < 0) return -1;
        offset += n;

        offset = json_append_kv_int(buf, offset, max_len, "cpu_cores", (int64_t)ncores);
        if (offset >= max_len) return -1;
    }

    int n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_log(char* buf, int max_len, const char* id_str, const char* params_json) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    /* 解析参数 */
    int64_t lines = MAX_LOG_LINES_DEFAULT;
    int64_t since_us = 0;  /* 增量拉取：只返回此时间戳之后的日志 */
    if (params_json && params_json[0] != '\0') {
        json_extract_int(params_json, "lines", &lines);
        json_extract_int(params_json, "since_us", &since_us);
        if (lines <= 0) lines = MAX_LOG_LINES_DEFAULT;
        if (lines > 500) lines = 500;
    }

    /* 获取最近日志 */
    CeLogEntry* log_buffer = (CeLogEntry*)malloc(sizeof(CeLogEntry) * (size_t)lines);
    uint32_t log_count = 0;
    uint64_t max_ts = 0;
    if (log_buffer) {
        log_count = ce_log_get_recent(log_buffer, (uint32_t)lines);
        /* 找到最大时间戳 */
        for (uint32_t i = 0; i < log_count; i++) {
            if (log_buffer[i].timestamp_us > max_ts) {
                max_ts = log_buffer[i].timestamp_us;
            }
        }
    }

    /* 输出 max_timestamp_us（供客户端下次 since_us 使用） */
    int n = snprintf(buf + offset, max_len - offset,
                     "\"max_timestamp_us\":%lu,\"entries\":[", (unsigned long)max_ts);
    if (n < 0) { free(log_buffer); return -1; }
    offset += n;

    int written = 0;
    for (uint32_t i = 0; i < log_count; i++) {
        /* 增量模式：跳过旧日志 */
        if (since_us > 0 && (int64_t)log_buffer[i].timestamp_us <= since_us) {
            continue;
        }
        if (written > 0) {
            n = snprintf(buf + offset, max_len - offset, ",");
            if (n < 0) { free(log_buffer); return -1; }
            offset += n;
        }
        written++;

        const char* level_str = "unknown";
        switch (log_buffer[i].level) {
        case CE_LOG_TRACE: level_str = "TRACE"; break;
        case CE_LOG_DEBUG: level_str = "DEBUG"; break;
        case CE_LOG_INFO:  level_str = "INFO";  break;
        case CE_LOG_WARN:  level_str = "WARN";  break;
        case CE_LOG_ERROR: level_str = "ERROR"; break;
        case CE_LOG_FATAL: level_str = "FATAL"; break;
        }

        n = snprintf(buf + offset, max_len - offset,
                     "{\"level\":\"%s\",\"timestamp_us\":%lu,"
                     "\"category\":",
                     level_str, (unsigned long)log_buffer[i].timestamp_us);
        if (n < 0) { free(log_buffer); return -1; }
        offset += n;

        offset = json_append_string(buf, offset, max_len,
                                     log_buffer[i].category ? log_buffer[i].category : "");
        if (offset >= max_len) { free(log_buffer); return -1; }

        n = snprintf(buf + offset, max_len - offset, ",\"message\":");
        if (n < 0) { free(log_buffer); return -1; }
        offset += n;

        offset = json_append_string(buf, offset, max_len,
                                     log_buffer[i].message ? log_buffer[i].message : "");
        if (offset >= max_len) { free(log_buffer); return -1; }

        n = snprintf(buf + offset, max_len - offset, ",\"file\":");
        if (n < 0) { free(log_buffer); return -1; }
        offset += n;

        offset = json_append_string(buf, offset, max_len,
                                     log_buffer[i].file ? log_buffer[i].file : "");
        if (offset >= max_len) { free(log_buffer); return -1; }

        n = snprintf(buf + offset, max_len - offset, ",\"line\":%d}",
                     log_buffer[i].line);
        if (n < 0) { free(log_buffer); return -1; }
        offset += n;
    }

    free(log_buffer);

    n = snprintf(buf + offset, max_len - offset, "]}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_render(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    CeRenderStats stats = ce_render_get_stats();

    offset = json_append_kv_int(buf, offset, max_len, "draw_calls", stats.draw_calls);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "triangles", stats.triangles);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "vertices", stats.vertices);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "frame_time_ms",
                                    (double)stats.frame_time_ms);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "gpu_time_ms",
                                    (double)stats.gpu_time_ms);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "backend", "vulkan");
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_system(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    const char* io_backend = ce_async_backend_name();
    CeBool ebpf_avail      = ce_ebpf_available();
    CeBool zcrx_supported  = ce_async_has_zcrx();
    pid_t   pid_val        = getpid();

    offset = json_append_kv_string(buf, offset, max_len, "engine_version", "0.1.0");
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "build_mode",
#ifdef NDEBUG
                                    "release"
#else
                                    "debug"
#endif
                                    );
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "io_backend",
                                    io_backend ? io_backend : "posix");
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_bool(buf, offset, max_len, "ebpf_available", ebpf_avail);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_bool(buf, offset, max_len, "zcrx_supported", zcrx_supported);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "compiler",
#ifdef __GNUC__
                                    "gcc"
#elif defined(__clang__)
                                    "clang"
#elif defined(_MSC_VER)
                                    "msvc"
#else
                                    "unknown"
#endif
                                    );
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "platform",
#ifdef __linux__
                                    "linux"
#elif defined(__APPLE__)
                                    "macos"
#elif defined(_WIN32)
                                    "windows"
#else
                                    "unknown"
#endif
                                    );
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_int(buf, offset, max_len, "pid", pid_val);
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

static int handle_health(char* buf, int max_len, const char* id_str) {
    int offset = json_rpc_result_start(buf, max_len, id_str);
    if (offset <= 0) return -1;

    CeEngineState state = ce_get_state();
    double uptime = ce_time_get_total();

    const char* state_str = "unknown";
    switch (state) {
    case CE_STATE_UNINITIALIZED: state_str = "uninitialized"; break;
    case CE_STATE_INITIALIZING:  state_str = "initializing";  break;
    case CE_STATE_RUNNING:       state_str = "running";       break;
    case CE_STATE_PAUSED:        state_str = "paused";        break;
    case CE_STATE_SHUTTING_DOWN: state_str = "shutting_down"; break;
    case CE_STATE_ERROR:         state_str = "error";         break;
    }

    offset = json_append_kv_bool(buf, offset, max_len, "ok",
                                  (state == CE_STATE_RUNNING) ? CE_TRUE : CE_FALSE);
    if (offset >= max_len) return -1;
    int n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_string(buf, offset, max_len, "state", state_str);
    if (offset >= max_len) return -1;
    n = snprintf(buf + offset, max_len - offset, ",");
    if (n < 0) return -1;
    offset += n;

    offset = json_append_kv_double(buf, offset, max_len, "uptime", uptime);
    if (offset >= max_len) return -1;

    n = snprintf(buf + offset, max_len - offset, "}}");
    if (n < 0) return -1;
    offset += n;

    return offset;
}

/* ---- 请求分发 ---- */

static int process_request(const char* request_json, char* response_buf, int max_len) {
    /* 提取 method */
    char method[128];
    if (json_extract_string(request_json, "method", method, sizeof(method)) < 0) {
        return json_rpc_error(response_buf, max_len, "null",
                              -32600, "Invalid Request: missing method");
    }

    /* 提取 id */
    char id_str[64] = "null";
    {
        char search[] = "\"id\"";
        const char* pos = strstr(request_json, search);
        if (pos) {
            pos += strlen(search);
            while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
            if (*pos == '"') {
                /* 字符串 id */
                pos++;
                int i = 0;
                while (*pos && *pos != '"' && i < 62) {
                    id_str[i++] = *pos++;
                }
                id_str[i] = '\0';
                /* 需要包装成带引号的形式 */
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "\"%s\"", id_str);
                memcpy(id_str, tmp, sizeof(id_str));
            } else if (*pos >= '0' && *pos <= '9') {
                /* 数字 id */
                int i = 0;
                while ((*pos >= '0' && *pos <= '9') && i < 62) {
                    id_str[i++] = *pos++;
                }
                id_str[i] = '\0';
            }
        }
    }

    /* 提取 params */
    char params_json[MAX_REQUEST_LEN] = "";
    {
        char search[] = "\"params\"";
        const char* pos = strstr(request_json, search);
        if (pos) {
            pos += strlen(search);
            while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
            if (*pos == '{') {
                /* 提取整个 params 对象 */
                int brace_depth = 0;
                int i = 0;
                const char* start = pos;
                while (*pos && i < MAX_REQUEST_LEN - 1) {
                    params_json[i++] = *pos;
                    if (*pos == '{') brace_depth++;
                    else if (*pos == '}') {
                        brace_depth--;
                        if (brace_depth == 0) {
                            pos++;
                            break;
                        }
                    }
                    pos++;
                }
                params_json[i] = '\0';
                (void)start;
            }
        }
    }

    /* 路由到具体方法 */
    if (strcmp(method, "stats") == 0) {
        return handle_stats(response_buf, max_len, id_str);
    } else if (strcmp(method, "aoi") == 0) {
        return handle_aoi(response_buf, max_len, id_str);
    } else if (strcmp(method, "cell") == 0) {
        return handle_cell(response_buf, max_len, id_str);
    } else if (strcmp(method, "network") == 0) {
        return handle_network(response_buf, max_len, id_str);
    } else if (strcmp(method, "memory") == 0) {
        return handle_memory(response_buf, max_len, id_str);
    } else if (strcmp(method, "cpu") == 0) {
        return handle_cpu(response_buf, max_len, id_str);
    } else if (strcmp(method, "log") == 0) {
        return handle_log(response_buf, max_len, id_str, params_json);
    } else if (strcmp(method, "render") == 0) {
        return handle_render(response_buf, max_len, id_str);
    } else if (strcmp(method, "system") == 0) {
        return handle_system(response_buf, max_len, id_str);
    } else if (strcmp(method, "health") == 0) {
        return handle_health(response_buf, max_len, id_str);
    }

    /* 尝试自定义处理器 */
    pthread_mutex_lock(&g_handler_mutex);
    for (int i = 0; i < g_custom_handler_count; i++) {
        if (g_custom_handlers[i]) {
            int result = g_custom_handlers[i](method, params_json,
                                               id_str, response_buf, max_len,
                                               g_custom_handler_data[i]);
            if (result > 0) {
                pthread_mutex_unlock(&g_handler_mutex);
                return result;
            }
        }
    }
    pthread_mutex_unlock(&g_handler_mutex);

    return json_rpc_error(response_buf, max_len, id_str,
                          -32601, "Method not found");
}

/* ---- 自定义处理器注册 ---- */

CeResult ce_admin_ipc_register_handler(CeAdminIpc* ipc,
                                        CeAdminIpcHandler handler,
                                        void* user_data) {
    (void)ipc; /* 全局注册，ipc 参数保留用于未来扩展 */
    if (!handler) return CE_ERR;

    pthread_mutex_lock(&g_handler_mutex);
    if (g_custom_handler_count >= MAX_CUSTOM_HANDLERS) {
        pthread_mutex_unlock(&g_handler_mutex);
        return CE_ERR;
    }
    g_custom_handlers[g_custom_handler_count] = handler;
    g_custom_handler_data[g_custom_handler_count] = user_data;
    g_custom_handler_count++;
    pthread_mutex_unlock(&g_handler_mutex);
    return CE_OK;
}

/* ---- Socket 操作 ---- */

static int create_listen_socket(const char* path) {
    int fd;
    struct sockaddr_un addr;

    /* 删除可能残留的 socket 文件 */
    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* 设置 socket 文件权限为 0666 */
    chmod(path, 0666);

    if (listen(fd, 1) < 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

static int recv_line(int fd, char* buf, int max_len) {
    int total = 0;
    while (total < max_len - 1) {
        ssize_t n = recv(fd, buf + total, 1, 0);
        if (n <= 0) {
            if (n == 0) {
                /* 连接关闭 */
                return (total > 0) ? total : -1;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        if (buf[total] == '\n') {
            buf[total] = '\0';
            /* 去除可能的 \r */
            if (total > 0 && buf[total - 1] == '\r') {
                buf[total - 1] = '\0';
                return total - 1;
            }
            return total;
        }
        total++;
    }
    buf[total] = '\0';
    return total;
}

static int send_response(int fd, const char* json) {
    size_t len = strlen(json);
    /* 发送 JSON + 换行 */
    char* buf = (char*)malloc(len + 2);
    if (!buf) return -1;

    memcpy(buf, json, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    size_t sent = 0;
    while (sent < len + 1) {
        ssize_t n = send(fd, buf + sent, len + 1 - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        sent += (size_t)n;
    }

    free(buf);
    return 0;
}

/* ---- 客户端处理 ---- */

static void handle_client(int client_fd) {
    char* request_buf  = (char*)malloc(MAX_REQUEST_LEN);
    char* response_buf = (char*)malloc(MAX_RESPONSE_LEN);

    if (!request_buf || !response_buf) {
        free(request_buf);
        free(response_buf);
        close(client_fd);
        return;
    }

    while (1) {
        int req_len = recv_line(client_fd, request_buf, MAX_REQUEST_LEN);
        if (req_len <= 0) {
            break; /* 连接关闭或错误 */
        }

        /* 跳过空行 */
        if (req_len == 0 || request_buf[0] == '\0') {
            continue;
        }

        int resp_len = process_request(request_buf, response_buf, MAX_RESPONSE_LEN);
        if (resp_len > 0) {
            response_buf[resp_len] = '\0';
            if (send_response(client_fd, response_buf) < 0) {
                break;
            }
        } else if (resp_len < 0) {
            /* 内部错误 */
            const char* err = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}";
            send_response(client_fd, err);
        }
    }

    free(request_buf);
    free(response_buf);
    close(client_fd);
}

/* ---- 线程主函数 ---- */

static void* ipc_thread_func(void* arg) {
    CeAdminIpc* ipc = (CeAdminIpc*)arg;

    /* 阻塞 SIGPIPE，防止写已关闭的 socket 时进程退出 */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    while (1) {
        /* 检查运行状态 */
        pthread_mutex_lock(&ipc->mutex);
        if (!ipc->running) {
            pthread_mutex_unlock(&ipc->mutex);
            break;
        }
        pthread_mutex_unlock(&ipc->mutex);

        /* 接受连接 */
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ipc->listen_fd,
                               (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            /* accept 错误，可能 socket 已关闭 */
            pthread_mutex_lock(&ipc->mutex);
            if (!ipc->running) {
                pthread_mutex_unlock(&ipc->mutex);
                break;
            }
            pthread_mutex_unlock(&ipc->mutex);
            continue;
        }

        /* 单连接模式：保存 client_fd */
        pthread_mutex_lock(&ipc->mutex);
        ipc->client_fd = client_fd;
        pthread_mutex_unlock(&ipc->mutex);

        /* 处理客户端请求（阻塞直到客户端断开） */
        handle_client(client_fd);

        /* 客户端断开 */
        pthread_mutex_lock(&ipc->mutex);
        ipc->client_fd = -1;
        pthread_mutex_unlock(&ipc->mutex);
    }

    return NULL;
}

/* ---- 公共 API ---- */

CeAdminIpc* ce_admin_ipc_start(const char* socket_path) {
    CeAdminIpc* ipc = (CeAdminIpc*)calloc(1, sizeof(CeAdminIpc));
    if (!ipc) return NULL;

    /* 设置 socket 路径 */
    if (socket_path && socket_path[0] != '\0') {
        strncpy(ipc->socket_path, socket_path, sizeof(ipc->socket_path) - 1);
    } else {
        strncpy(ipc->socket_path, DEFAULT_SOCKET_PATH, sizeof(ipc->socket_path) - 1);
    }
    ipc->socket_path[sizeof(ipc->socket_path) - 1] = '\0';

    /* 创建监听 socket */
    ipc->listen_fd = create_listen_socket(ipc->socket_path);
    if (ipc->listen_fd < 0) {
        free(ipc);
        return NULL;
    }

    ipc->client_fd = -1;
    ipc->running   = CE_TRUE;

    /* 初始化互斥锁 */
    if (pthread_mutex_init(&ipc->mutex, NULL) != 0) {
        close(ipc->listen_fd);
        unlink(ipc->socket_path);
        free(ipc);
        return NULL;
    }

    /* 启动工作线程 */
    if (pthread_create(&ipc->thread, NULL, ipc_thread_func, ipc) != 0) {
        pthread_mutex_destroy(&ipc->mutex);
        close(ipc->listen_fd);
        unlink(ipc->socket_path);
        free(ipc);
        return NULL;
    }

    return ipc;
}

void ce_admin_ipc_stop(CeAdminIpc* ipc) {
    if (!ipc) return;

    /* 标记停止 */
    pthread_mutex_lock(&ipc->mutex);
    ipc->running = CE_FALSE;
    /* 关闭 client 连接以唤醒 accept */
    if (ipc->client_fd >= 0) {
        close(ipc->client_fd);
        ipc->client_fd = -1;
    }
    pthread_mutex_unlock(&ipc->mutex);

    /* 关闭监听 socket 以唤醒 accept */
    if (ipc->listen_fd >= 0) {
        close(ipc->listen_fd);
        ipc->listen_fd = -1;
    }

    /* 等待线程退出 */
    pthread_join(ipc->thread, NULL);

    /* 清理 */
    pthread_mutex_destroy(&ipc->mutex);
    unlink(ipc->socket_path);
    free(ipc);
}

CeBool ce_admin_ipc_is_running(CeAdminIpc* ipc) {
    if (!ipc) return CE_FALSE;
    CeBool running;
    pthread_mutex_lock(&ipc->mutex);
    running = ipc->running;
    pthread_mutex_unlock(&ipc->mutex);
    return running;
}
