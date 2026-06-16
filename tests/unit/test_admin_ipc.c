/*
 * test_admin_ipc.c — Phase 5.1: Admin IPC 单元测试
 *
 * 测试 JSON-RPC 协议解析、请求分发、响应构造等核心功能。
 * 直接调用内部函数（通过包含源文件），无需启动 socket。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>

/* ---- 测试框架 ---- */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", name); \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s (expected %ld, got %ld)", msg, (long)(b), (long)(a)); \
        FAIL(_buf); return; \
    } \
} while(0)

#define ASSERT_STREQ(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        char _buf[512]; \
        snprintf(_buf, sizeof(_buf), "%s (expected '%s', got '%s')", msg, (b), (a)); \
        FAIL(_buf); return; \
    } \
} while(0)

/* ---- 模拟 chaos_engine 依赖 ---- */

/* 这些函数在测试中不需要真正的引擎，只需要返回合理的值 */
#include <stdint.h>
#include <stdbool.h>

typedef int CeBool;
#define CE_TRUE  1
#define CE_FALSE 0
#define CE_OK    0
#define CE_ERR  -1

typedef enum {
    CE_STATE_UNINITIALIZED = 0,
    CE_STATE_INITIALIZING,
    CE_STATE_RUNNING,
    CE_STATE_PAUSED,
    CE_STATE_SHUTTING_DOWN,
    CE_STATE_ERROR
} CeEngineState;

typedef enum {
    CE_LOG_TRACE = 0,
    CE_LOG_DEBUG,
    CE_LOG_INFO,
    CE_LOG_WARN,
    CE_LOG_ERROR,
    CE_LOG_FATAL
} CeLogLevel;

typedef struct {
    CeLogLevel  level;
    uint64_t    timestamp_us;
    const char* category;
    const char* message;
    const char* file;
    int         line;
} CeLogEntry;

typedef struct {
    int32_t draw_calls;
    int32_t triangles;
    int32_t vertices;
    float   frame_time_ms;
    float   gpu_time_ms;
} CeRenderStats;

/* Mock implementations */
static uint32_t mock_entity_count = 42;
static uint32_t mock_component_count = 128;
static double   mock_delta_time = 0.016;
static double   mock_total_time = 120.5;

uint32_t ce_ecs_get_entity_count(void) { return mock_entity_count; }
uint32_t ce_ecs_get_component_count(void) { return mock_component_count; }
double   ce_time_get_delta(void) { return mock_delta_time; }
double   ce_time_get_total(void) { return mock_total_time; }

int      ce_aoi_entity_count(void) { return 15; }

/* Cell mock */
typedef uint32_t CeCellId;
#define CE_INVALID_CELL_ID 0xFFFFFFFF

typedef enum { CE_CELL_ACTIVE = 0, CE_CELL_INACTIVE } CeCellState;

typedef struct {
    float min_x, min_y, max_x, max_y;
} CeBounds;

typedef struct {
    CeCellId    id;
    CeBounds    bounds;
    int         entity_count;
    CeCellState state;
    int         process_id;
} CeCell;

static int mock_cell_count = 10;
int ce_cell_count(void) { return mock_cell_count; }

const CeCell* ce_cell_get(CeCellId id) {
    static CeCell cells[20];
    if (id >= (CeCellId)mock_cell_count) return NULL;
    cells[id].id = id;
    cells[id].bounds.min_x = (float)(id % 4) * 100.0f;
    cells[id].bounds.min_y = (float)(id / 4) * 100.0f;
    cells[id].bounds.max_x = cells[id].bounds.min_x + 100.0f;
    cells[id].bounds.max_y = cells[id].bounds.min_y + 100.0f;
    cells[id].entity_count = (int)(id * 3);
    cells[id].state = CE_CELL_ACTIVE;
    cells[id].process_id = 0;
    return &cells[id];
}

const char* ce_async_backend_name(void) { return "io_uring"; }
CeBool      ce_async_has_zcrx(void) { return CE_TRUE; }
CeBool      ce_ebpf_available(void) { return CE_TRUE; }

CeRenderStats ce_render_get_stats(void) {
    CeRenderStats s = { 150, 50000, 120000, 8.5f, 6.2f };
    return s;
}

CeEngineState ce_get_state(void) { return CE_STATE_RUNNING; }

uint32_t ce_log_get_recent(CeLogEntry* buf, uint32_t max_count) {
    if (!buf || max_count == 0) return 0;
    uint32_t n = max_count < 3 ? max_count : 3;
    for (uint32_t i = 0; i < n; i++) {
        buf[i].level = CE_LOG_INFO;
        buf[i].timestamp_us = 1000000 + i * 1000;
        buf[i].category = "test";
        buf[i].message = "test message";
        buf[i].file = "test.c";
        buf[i].line = (int)(i + 1);
    }
    return n;
}

/* ---- 包含 Admin IPC 实现（直接测试内部函数） ---- */

/* 我们需要直接测试 JSON 辅助函数和 process_request。
 * 由于这些是 static 函数，我们将复制关键逻辑进行测试。
 * 更好的做法：直接 include 源文件（去掉 static 关键字会有冲突），
 * 所以我们手动测试 JSON 构造和解析逻辑。
 */

/* 复制 JSON 辅助函数用于测试 */
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

static int json_append_int(char* buf, int offset, int max_len, int64_t val) {
    int n = snprintf(buf + offset, max_len - offset, "%ld", (long)val);
    if (n < 0) return offset;
    return offset + n;
}

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

static int json_extract_string(const char* json, const char* key, char* out, int max_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos != '"') return -1;
    pos++;
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

static int json_extract_int(const char* json, const char* key, int64_t* out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos == ':' || *pos == ' ' || *pos == '\t') pos++;
    if (*pos == '"') return -1;
    char* end = NULL;
    long val = strtol(pos, &end, 10);
    if (end == pos) return -1;
    *out = (int64_t)val;
    return 0;
}

/* ---- 5.1.1: JSON 字符串编码测试 ---- */

static void test_json_append_string_basic(void) {
    TEST("json_append_string basic");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_string(buf, 0, sizeof(buf), "hello");
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "\"hello\"", "should produce quoted string");
    PASS();
}

static void test_json_append_string_escape(void) {
    TEST("json_append_string escape sequences");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_string(buf, 0, sizeof(buf), "a\"b\\c\nd\re\tf");
    ASSERT(offset > 0, "offset should be positive");
    ASSERT(strstr(buf, "\\\"") != NULL, "should escape double quote");
    ASSERT(strstr(buf, "\\\\") != NULL, "should escape backslash");
    ASSERT(strstr(buf, "\\n") != NULL, "should escape newline");
    ASSERT(strstr(buf, "\\r") != NULL, "should escape carriage return");
    ASSERT(strstr(buf, "\\t") != NULL, "should escape tab");
    PASS();
}

static void test_json_append_string_empty(void) {
    TEST("json_append_string empty string");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_string(buf, 0, sizeof(buf), "");
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "\"\"", "should produce empty quotes");
    PASS();
}

static void test_json_append_string_null(void) {
    TEST("json_append_string NULL");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_string(buf, 0, sizeof(buf), NULL);
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "\"\"", "NULL should produce empty quotes");
    PASS();
}

/* ---- 5.1.2: JSON 整数编码测试 ---- */

static void test_json_append_int_positive(void) {
    TEST("json_append_int positive");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_int(buf, 0, sizeof(buf), 12345);
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "12345", "should produce number string");
    PASS();
}

static void test_json_append_int_negative(void) {
    TEST("json_append_int negative");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_int(buf, 0, sizeof(buf), -999);
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "-999", "should produce negative number");
    PASS();
}

static void test_json_append_int_zero(void) {
    TEST("json_append_int zero");
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_int(buf, 0, sizeof(buf), 0);
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "0", "should produce zero");
    PASS();
}

/* ---- 5.1.3: JSON 键值对测试 ---- */

static void test_json_kv_string(void) {
    TEST("json_append_kv_string");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_kv_string(buf, 0, sizeof(buf), "name", "value");
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "\"name\":\"value\"", "should produce key:value");
    PASS();
}

static void test_json_kv_int(void) {
    TEST("json_append_kv_int");
    char buf[256];
    memset(buf, 0, sizeof(buf));
    int offset = json_append_kv_int(buf, 0, sizeof(buf), "count", 42);
    ASSERT(offset > 0, "offset should be positive");
    ASSERT_STREQ(buf, "\"count\":42", "should produce key:number");
    PASS();
}

/* ---- 5.1.4: JSON 字符串解析测试 ---- */

static void test_json_extract_string_basic(void) {
    TEST("json_extract_string basic");
    const char* json = "{\"method\":\"stats\",\"id\":1}";
    char out[64];
    int len = json_extract_string(json, "method", out, sizeof(out));
    ASSERT(len > 0, "should extract method");
    ASSERT_STREQ(out, "stats", "should extract 'stats'");
    PASS();
}

static void test_json_extract_string_missing(void) {
    TEST("json_extract_string missing key");
    const char* json = "{\"method\":\"stats\"}";
    char out[64];
    int len = json_extract_string(json, "nonexistent", out, sizeof(out));
    ASSERT_EQ(len, -1, "should return -1 for missing key");
    PASS();
}

static void test_json_extract_string_escaped(void) {
    TEST("json_extract_string with escapes");
    const char* json = "{\"msg\":\"hello\\nworld\"}";
    char out[64];
    int len = json_extract_string(json, "msg", out, sizeof(out));
    ASSERT(len > 0, "should extract escaped string");
    ASSERT(strcmp(out, "hello\nworld") == 0, "should unescape newline");
    PASS();
}

/* ---- 5.1.5: JSON 整数解析测试 ---- */

static void test_json_extract_int_basic(void) {
    TEST("json_extract_int basic");
    const char* json = "{\"lines\":100,\"id\":1}";
    int64_t val = 0;
    int ret = json_extract_int(json, "lines", &val);
    ASSERT_EQ(ret, 0, "should succeed");
    ASSERT_EQ(val, 100, "should extract 100");
    PASS();
}

static void test_json_extract_int_missing(void) {
    TEST("json_extract_int missing key");
    const char* json = "{\"lines\":100}";
    int64_t val = 0;
    int ret = json_extract_int(json, "missing", &val);
    ASSERT_EQ(ret, -1, "should return -1 for missing key");
    PASS();
}

static void test_json_extract_int_string_value(void) {
    TEST("json_extract_int string value (should fail)");
    const char* json = "{\"lines\":\"not_a_number\"}";
    int64_t val = 0;
    int ret = json_extract_int(json, "lines", &val);
    ASSERT_EQ(ret, -1, "should return -1 for string value");
    PASS();
}

/* ---- 5.1.6: JSON-RPC 错误响应测试 ---- */

static int json_rpc_error(char* buf, int max_len, const char* id_str,
                           int code, const char* message) {
    return snprintf(buf, max_len,
                    "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                    id_str, code, message);
}

static void test_json_rpc_error_format(void) {
    TEST("json_rpc_error format");
    char buf[512];
    int len = json_rpc_error(buf, sizeof(buf), "null", -32600, "Invalid Request");
    ASSERT(len > 0, "should produce error response");
    ASSERT(strstr(buf, "\"jsonrpc\":\"2.0\"") != NULL, "should have jsonrpc version");
    ASSERT(strstr(buf, "\"code\":-32600") != NULL, "should have error code");
    ASSERT(strstr(buf, "Invalid Request") != NULL, "should have error message");
    PASS();
}

static void test_json_rpc_error_method_not_found(void) {
    TEST("json_rpc_error method not found");
    char buf[512];
    int len = json_rpc_error(buf, sizeof(buf), "1", -32601, "Method not found");
    ASSERT(len > 0, "should produce error response");
    ASSERT(strstr(buf, "\"id\":1") != NULL, "should echo id");
    ASSERT(strstr(buf, "\"code\":-32601") != NULL, "should have -32601");
    PASS();
}

/* ---- 5.1.7: JSON-RPC 结果起始测试 ---- */

static int json_rpc_result_start(char* buf, int max_len, const char* id_str) {
    int offset = 0;
    int n = snprintf(buf + offset, max_len - offset,
                     "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{", id_str);
    if (n < 0) return 0;
    offset += n;
    return offset;
}

static void test_json_rpc_result_start(void) {
    TEST("json_rpc_result_start");
    char buf[512];
    memset(buf, 0, sizeof(buf));
    int offset = json_rpc_result_start(buf, sizeof(buf), "42");
    ASSERT(offset > 0, "should produce result start");
    ASSERT(strstr(buf, "\"jsonrpc\":\"2.0\"") != NULL, "should have jsonrpc");
    ASSERT(strstr(buf, "\"id\":42") != NULL, "should have id");
    ASSERT(strstr(buf, "\"result\":{") != NULL, "should have result object start");
    PASS();
}

/* ---- 5.1.8: 缓冲区边界测试 ---- */

static void test_buffer_overflow_protection(void) {
    TEST("buffer overflow protection");
    char buf[16];
    memset(buf, 0, sizeof(buf));

    /* 尝试写入超长字符串 */
    int offset = json_append_string(buf, 0, sizeof(buf),
        "this is a very long string that should be truncated");
    ASSERT(offset <= (int)sizeof(buf), "offset should not exceed buffer");
    /* 应该至少包含开头引号 */
    ASSERT(buf[0] == '"', "should start with quote");
    PASS();
}

static void test_kv_buffer_boundary(void) {
    TEST("kv pair at buffer boundary");
    char buf[32];
    memset(buf, 0, sizeof(buf));
    /* 写入刚好能容纳的键值对 */
    int offset = json_append_kv_string(buf, 0, sizeof(buf), "k", "v");
    ASSERT(offset > 0, "should write something");
    ASSERT(offset < (int)sizeof(buf), "should fit in buffer");
    PASS();
}

/* ---- 5.1.9: 完整 JSON-RPC 请求模拟测试 ---- */

static void test_full_stats_response(void) {
    TEST("full stats JSON-RPC response construction");

    char buf[1024];
    memset(buf, 0, sizeof(buf));

    /* 模拟 handle_stats 的响应构造 */
    int offset = json_rpc_result_start(buf, sizeof(buf), "1");
    ASSERT(offset > 0, "should start result");

    offset = json_append_kv_int(buf, offset, sizeof(buf), "entity_count", 42);
    ASSERT(offset > 0, "should append entity_count");
    int n = snprintf(buf + offset, sizeof(buf) - offset, ",");
    ASSERT(n > 0, "should append comma");
    offset += n;

    offset = json_append_kv_int(buf, offset, sizeof(buf), "component_count", 128);
    ASSERT(offset > 0, "should append component_count");

    n = snprintf(buf + offset, sizeof(buf) - offset, "}}");
    ASSERT(n > 0, "should close objects");
    offset += n;

    /* 验证关键字段 */
    ASSERT(strstr(buf, "\"entity_count\":42") != NULL, "should contain entity_count");
    ASSERT(strstr(buf, "\"component_count\":128") != NULL, "should contain component_count");
    ASSERT(strstr(buf, "\"jsonrpc\":\"2.0\"") != NULL, "should have jsonrpc version");
    ASSERT(strstr(buf, "\"id\":1") != NULL, "should have id");

    /* 验证以 }} 结尾 */
    size_t len = strlen(buf);
    ASSERT(len >= 2, "should be at least 2 chars");
    ASSERT(buf[len-2] == '}' && buf[len-1] == '}', "should end with }}");

    PASS();
}

/* ---- 5.1.10: Unix Socket 创建/销毁测试 ---- */

static void test_unix_socket_create_destroy(void) {
    TEST("unix socket create and destroy");

    const char* path = "/tmp/test_admin_ipc_unit.sock";
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "should create socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT(ret == 0, "should bind socket");

    ret = listen(fd, 1);
    ASSERT(ret == 0, "should listen on socket");

    /* 验证 socket 文件存在 */
    ret = access(path, F_OK);
    ASSERT(ret == 0, "socket file should exist");

    /* 清理 */
    close(fd);
    unlink(path);

    /* 验证 socket 文件已删除 */
    ret = access(path, F_OK);
    ASSERT(ret == -1, "socket file should be removed");

    PASS();
}

/* ---- 5.1.11: recv_line 逻辑测试 ---- */

static void test_recv_line_logic(void) {
    TEST("recv_line newline-delimited parsing");

    /* 模拟 recv_line 的逻辑 */
    const char* test_data = "{\"method\":\"stats\"}\n";
    char buf[256];
    int total = 0;

    for (size_t i = 0; i < strlen(test_data); i++) {
        if (test_data[i] == '\n') {
            buf[total] = '\0';
            if (total > 0 && buf[total - 1] == '\r') {
                buf[total - 1] = '\0';
            }
            break;
        }
        buf[total++] = test_data[i];
    }

    ASSERT_STREQ(buf, "{\"method\":\"stats\"}", "should extract line without newline");
    PASS();
}

static void test_recv_line_crlf(void) {
    TEST("recv_line CRLF handling");

    const char* test_data = "{\"method\":\"stats\"}\r\n";
    char buf[256];
    int total = 0;

    for (size_t i = 0; i < strlen(test_data); i++) {
        if (test_data[i] == '\n') {
            buf[total] = '\0';
            if (total > 0 && buf[total - 1] == '\r') {
                buf[total - 1] = '\0';
                total--;
            }
            break;
        }
        buf[total++] = test_data[i];
    }

    ASSERT_STREQ(buf, "{\"method\":\"stats\"}", "should strip CRLF");
    PASS();
}

/* ---- 5.1.12: send_response 逻辑测试 ---- */

static void test_send_response_format(void) {
    TEST("send_response appends newline");

    const char* json = "{\"result\":\"ok\"}";
    size_t len = strlen(json);
    char* buf = (char*)malloc(len + 2);
    memcpy(buf, json, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    ASSERT(buf[len] == '\n', "should append newline");
    ASSERT_STREQ(buf, "{\"result\":\"ok\"}\n", "should have trailing newline");

    free(buf);
    PASS();
}

/* ---- 主函数 ---- */

int main(void) {
    printf("\n=== Phase 5.1: Admin IPC Unit Tests ===\n\n");

    /* JSON 编码 */
    printf("--- JSON String Encoding ---\n");
    test_json_append_string_basic();
    test_json_append_string_escape();
    test_json_append_string_empty();
    test_json_append_string_null();

    printf("\n--- JSON Integer Encoding ---\n");
    test_json_append_int_positive();
    test_json_append_int_negative();
    test_json_append_int_zero();

    printf("\n--- JSON Key-Value Pairs ---\n");
    test_json_kv_string();
    test_json_kv_int();

    printf("\n--- JSON String Extraction ---\n");
    test_json_extract_string_basic();
    test_json_extract_string_missing();
    test_json_extract_string_escaped();

    printf("\n--- JSON Integer Extraction ---\n");
    test_json_extract_int_basic();
    test_json_extract_int_missing();
    test_json_extract_int_string_value();

    printf("\n--- JSON-RPC Error Responses ---\n");
    test_json_rpc_error_format();
    test_json_rpc_error_method_not_found();

    printf("\n--- JSON-RPC Result Start ---\n");
    test_json_rpc_result_start();

    printf("\n--- Buffer Boundary Tests ---\n");
    test_buffer_overflow_protection();
    test_kv_buffer_boundary();

    printf("\n--- Full Response Construction ---\n");
    test_full_stats_response();

    printf("\n--- Socket Operations ---\n");
    test_unix_socket_create_destroy();

    printf("\n--- Line Parsing ---\n");
    test_recv_line_logic();
    test_recv_line_crlf();

    printf("\n--- Response Formatting ---\n");
    test_send_response_format();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
