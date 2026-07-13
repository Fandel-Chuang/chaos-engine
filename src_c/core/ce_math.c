/*
 * ChaosEngine 数学库
 * 纯 C 实现，无平台依赖
 */

#include "core/ce_math.h"
#include <math.h>
#include <string.h>

/* ---- 向量 ---- */

CeVec2 ce_vec2_zero(void) {
    return (CeVec2){0, 0};
}

CeVec2 ce_vec2_add(CeVec2 a, CeVec2 b) {
    return (CeVec2){a.x + b.x, a.y + b.y};
}

CeVec2 ce_vec2_sub(CeVec2 a, CeVec2 b) {
    return (CeVec2){a.x - b.x, a.y - b.y};
}

CeVec2 ce_vec2_scale(CeVec2 v, float s) {
    return (CeVec2){v.x * s, v.y * s};
}

float ce_vec2_dot(CeVec2 a, CeVec2 b) {
    return a.x * b.x + a.y * b.y;
}

float ce_vec2_length(CeVec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

CeVec2 ce_vec2_normalize(CeVec2 v) {
    float len = ce_vec2_length(v);
    if (len < 0.000001f) return ce_vec2_zero();
    return ce_vec2_scale(v, 1.0f / len);
}

CeVec3 ce_vec3_zero(void) {
    return (CeVec3){0, 0, 0};
}

CeVec3 ce_vec3_add(CeVec3 a, CeVec3 b) {
    return (CeVec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

CeVec3 ce_vec3_sub(CeVec3 a, CeVec3 b) {
    return (CeVec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

CeVec3 ce_vec3_scale(CeVec3 v, float s) {
    return (CeVec3){v.x * s, v.y * s, v.z * s};
}

float ce_vec3_dot(CeVec3 a, CeVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

CeVec3 ce_vec3_cross(CeVec3 a, CeVec3 b) {
    return (CeVec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float ce_vec3_length(CeVec3 v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

CeVec3 ce_vec3_normalize(CeVec3 v) {
    float len = ce_vec3_length(v);
    if (len < 0.000001f) return ce_vec3_zero();
    return ce_vec3_scale(v, 1.0f / len);
}

CeVec4 ce_vec4_zero(void) {
    return (CeVec4){0, 0, 0, 0};
}

/* ---- 矩阵 ---- */

CeMat4 ce_mat4_identity(void) {
    CeMat4 m;
    memset(m.m, 0, sizeof(m.m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

/* near/far are Windows SDK macros — undef locally for this function */
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
CeMat4 ce_mat4_perspective(float fov_y, float aspect, float near, float far) {
    CeMat4 m;
    memset(m.m, 0, sizeof(m.m));
    float tan_half_fov = tanf(fov_y * 0.5f);
    m.m[0]  = 1.0f / (aspect * tan_half_fov);
    m.m[5]  = 1.0f / tan_half_fov;
    m.m[10] = -(far + near) / (far - near);
    m.m[11] = -1.0f;
    m.m[14] = -(2.0f * far * near) / (far - near);
    return m;
}

CeMat4 ce_mat4_look_at(CeVec3 eye, CeVec3 target, CeVec3 up) {
    CeVec3 f = ce_vec3_normalize(ce_vec3_sub(target, eye));
    CeVec3 s = ce_vec3_normalize(ce_vec3_cross(f, up));
    CeVec3 u = ce_vec3_cross(s, f);

    CeMat4 m = ce_mat4_identity();
    m.m[0] = s.x;  m.m[1] = u.x;  m.m[2]  = -f.x;
    m.m[4] = s.y;  m.m[5] = u.y;  m.m[6]  = -f.y;
    m.m[8] = s.z;  m.m[9] = u.z;  m.m[10] = -f.z;
    m.m[12] = -ce_vec3_dot(s, eye);
    m.m[13] = -ce_vec3_dot(u, eye);
    m.m[14] =  ce_vec3_dot(f, eye);
    return m;
}

CeMat4 ce_mat4_translation(CeVec3 pos) {
    CeMat4 m = ce_mat4_identity();
    m.m[12] = pos.x;
    m.m[13] = pos.y;
    m.m[14] = pos.z;
    return m;
}

CeMat4 ce_mat4_rotation(CeQuat q) {
    CeMat4 m = ce_mat4_identity();
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    m.m[0] = 1.0f - 2.0f * (yy + zz);
    m.m[1] = 2.0f * (xy + wz);
    m.m[2] = 2.0f * (xz - wy);

    m.m[4] = 2.0f * (xy - wz);
    m.m[5] = 1.0f - 2.0f * (xx + zz);
    m.m[6] = 2.0f * (yz + wx);

    m.m[8] = 2.0f * (xz + wy);
    m.m[9] = 2.0f * (yz - wx);
    m.m[10] = 1.0f - 2.0f * (xx + yy);

    return m;
}

CeMat4 ce_mat4_scale(CeVec3 scale) {
    CeMat4 m = ce_mat4_identity();
    m.m[0] = scale.x;
    m.m[5] = scale.y;
    m.m[10] = scale.z;
    return m;
}

CeMat4 ce_mat4_mul(CeMat4 a, CeMat4 b) {
    CeMat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r.m[col * 4 + row] =
                a.m[row]      * b.m[col * 4] +
                a.m[row + 4]  * b.m[col * 4 + 1] +
                a.m[row + 8]  * b.m[col * 4 + 2] +
                a.m[row + 12] * b.m[col * 4 + 3];
        }
    }
    return r;
}

CeMat4 ce_mat4_trs(CeVec3 pos, CeQuat rot, CeVec3 scale) {
    CeMat4 t = ce_mat4_translation(pos);
    CeMat4 r = ce_mat4_rotation(rot);
    CeMat4 s = ce_mat4_scale(scale);
    return ce_mat4_mul(ce_mat4_mul(t, r), s);
}

/* ---- 四元数 ---- */

CeQuat ce_quat_identity(void) {
    return (CeQuat){0, 0, 0, 1};
}

CeQuat ce_quat_euler(float pitch, float yaw, float roll) {
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    float cy = cosf(yaw   * 0.5f), sy = sinf(yaw   * 0.5f);
    float cr = cosf(roll  * 0.5f), sr = sinf(roll  * 0.5f);

    return (CeQuat){
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy
    };
}

/* ---- 颜色 ---- */

CeColor ce_color_rgba(float r, float g, float b, float a) {
    return (CeColor){r, g, b, a};
}

CeColor ce_color_white(void)  { return (CeColor){1, 1, 1, 1}; }
CeColor ce_color_black(void)  { return (CeColor){0, 0, 0, 1}; }
CeColor ce_color_red(void)    { return (CeColor){1, 0, 0, 1}; }
CeColor ce_color_green(void)  { return (CeColor){0, 1, 0, 1}; }
CeColor ce_color_blue(void)   { return (CeColor){0, 0, 1, 1}; }

/* ---- 工具函数 ---- */

float ce_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float ce_clamp(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

float ce_radians(float degrees) {
    return degrees * 0.01745329252f; /* PI / 180 */
}

float ce_degrees(float radians) {
    return radians * 57.295779513f; /* 180 / PI */
}
