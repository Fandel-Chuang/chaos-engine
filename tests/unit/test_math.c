/*
 * ChaosEngine 数学库单元测试
 * 测试: ce_vec3_add/sub/dot/cross/length/normalize, ce_mat4_identity/mul, ce_quat_euler
 */

#include "public_api/ce_types.h"
#include "core/ce_math.h"
#include <stdio.h>
#include <math.h>

#define TEST(name) printf("  TEST: %s ... ", name)
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while(0)
#define PASS() printf("PASS\n")

static int float_eq(float a, float b) {
    return fabsf(a - b) < 0.0001f;
}

int main(void) {
    int failures = 0;

    printf("=== Math Tests ===\n");

    /* ---- vec3_add ---- */
    TEST("vec3_add");
    {
        CeVec3 a = {1.0f, 2.0f, 3.0f};
        CeVec3 b = {4.0f, 5.0f, 6.0f};
        CeVec3 r = ce_vec3_add(a, b);
        CHECK(float_eq(r.x, 5.0f) && float_eq(r.y, 7.0f) && float_eq(r.z, 9.0f));
    }
    PASS();

    /* ---- vec3_sub ---- */
    TEST("vec3_sub");
    {
        CeVec3 a = {5.0f, 7.0f, 9.0f};
        CeVec3 b = {1.0f, 2.0f, 3.0f};
        CeVec3 r = ce_vec3_sub(a, b);
        CHECK(float_eq(r.x, 4.0f) && float_eq(r.y, 5.0f) && float_eq(r.z, 6.0f));
    }
    PASS();

    /* ---- vec3_dot ---- */
    TEST("vec3_dot");
    {
        CeVec3 a = {1.0f, 0.0f, 0.0f};
        CeVec3 b = {0.0f, 1.0f, 0.0f};
        float d = ce_vec3_dot(a, b);
        CHECK(float_eq(d, 0.0f));
    }
    PASS();

    TEST("vec3_dot_parallel");
    {
        CeVec3 a = {2.0f, 3.0f, 4.0f};
        float d = ce_vec3_dot(a, a);
        CHECK(float_eq(d, 29.0f)); /* 4+9+16 = 29 */
    }
    PASS();

    /* ---- vec3_cross ---- */
    TEST("vec3_cross");
    {
        CeVec3 a = {1.0f, 0.0f, 0.0f};
        CeVec3 b = {0.0f, 1.0f, 0.0f};
        CeVec3 r = ce_vec3_cross(a, b);
        CHECK(float_eq(r.x, 0.0f) && float_eq(r.y, 0.0f) && float_eq(r.z, 1.0f));
    }
    PASS();

    /* ---- vec3_length ---- */
    TEST("vec3_length");
    {
        CeVec3 v = {3.0f, 4.0f, 0.0f};
        float len = ce_vec3_length(v);
        CHECK(float_eq(len, 5.0f));
    }
    PASS();

    /* ---- vec3_normalize ---- */
    TEST("vec3_normalize");
    {
        CeVec3 v = {3.0f, 0.0f, 4.0f};
        CeVec3 n = ce_vec3_normalize(v);
        CHECK(float_eq(n.x, 0.6f) && float_eq(n.y, 0.0f) && float_eq(n.z, 0.8f));
        /* 验证归一化后长度为 1 */
        float len = ce_vec3_length(n);
        CHECK(float_eq(len, 1.0f));
    }
    PASS();

    TEST("vec3_normalize_zero");
    {
        CeVec3 v = {0.0f, 0.0f, 0.0f};
        CeVec3 n = ce_vec3_normalize(v);
        CHECK(float_eq(n.x, 0.0f) && float_eq(n.y, 0.0f) && float_eq(n.z, 0.0f));
    }
    PASS();

    /* ---- mat4_identity ---- */
    TEST("mat4_identity");
    {
        CeMat4 m = ce_mat4_identity();
        CHECK(float_eq(m.m[0], 1.0f) && float_eq(m.m[5], 1.0f) &&
              float_eq(m.m[10], 1.0f) && float_eq(m.m[15], 1.0f));
        CHECK(float_eq(m.m[1], 0.0f) && float_eq(m.m[2], 0.0f) &&
              float_eq(m.m[3], 0.0f) && float_eq(m.m[4], 0.0f));
    }
    PASS();

    /* ---- mat4_mul ---- */
    TEST("mat4_mul_identity");
    {
        CeMat4 id = ce_mat4_identity();
        CeMat4 r = ce_mat4_mul(id, id);
        for (int i = 0; i < 16; i++) {
            CHECK(float_eq(r.m[i], id.m[i]));
        }
    }
    PASS();

    TEST("mat4_mul_translation");
    {
        CeVec3 pos = {1.0f, 2.0f, 3.0f};
        CeMat4 t = ce_mat4_translation(pos);
        CeMat4 id = ce_mat4_identity();
        CeMat4 r = ce_mat4_mul(id, t);
        CHECK(float_eq(r.m[12], 1.0f) && float_eq(r.m[13], 2.0f) && float_eq(r.m[14], 3.0f));
    }
    PASS();

    /* ---- quat_euler ---- */
    TEST("quat_euler_zero");
    {
        CeQuat q = ce_quat_euler(0.0f, 0.0f, 0.0f);
        CHECK(float_eq(q.x, 0.0f) && float_eq(q.y, 0.0f) &&
              float_eq(q.z, 0.0f) && float_eq(q.w, 1.0f));
    }
    PASS();

    TEST("quat_euler_nonzero");
    {
        /* 绕 Y 轴旋转 90 度 */
        CeQuat q = ce_quat_euler(0.0f, 1.5707963f, 0.0f); /* yaw = PI/2 */
        /* 四元数不应全为零 */
        CHECK(!(float_eq(q.x, 0.0f) && float_eq(q.y, 0.0f) &&
                float_eq(q.z, 0.0f) && float_eq(q.w, 0.0f)));
        /* 验证是单位四元数 */
        float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
        CHECK(float_eq(len, 1.0f));
    }
    PASS();

    printf("\nAll math tests passed!\n");
    return 0;
}
