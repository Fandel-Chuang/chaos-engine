/*
 * ChaosEngine 数学库头文件
 */

#ifndef CE_MATH_H
#define CE_MATH_H

#include "public_api/ce_types.h"

/* ---- 向量 ---- */
CeVec2 ce_vec2_zero(void);
CeVec2 ce_vec2_add(CeVec2 a, CeVec2 b);
CeVec2 ce_vec2_sub(CeVec2 a, CeVec2 b);
CeVec2 ce_vec2_scale(CeVec2 v, float s);
float  ce_vec2_dot(CeVec2 a, CeVec2 b);
float  ce_vec2_length(CeVec2 v);
CeVec2 ce_vec2_normalize(CeVec2 v);

CeVec3 ce_vec3_zero(void);
CeVec3 ce_vec3_add(CeVec3 a, CeVec3 b);
CeVec3 ce_vec3_sub(CeVec3 a, CeVec3 b);
CeVec3 ce_vec3_scale(CeVec3 v, float s);
float  ce_vec3_dot(CeVec3 a, CeVec3 b);
CeVec3 ce_vec3_cross(CeVec3 a, CeVec3 b);
float  ce_vec3_length(CeVec3 v);
CeVec3 ce_vec3_normalize(CeVec3 v);

CeVec4 ce_vec4_zero(void);

/* ---- 矩阵 ---- */
CeMat4 ce_mat4_identity(void);
CeMat4 ce_mat4_perspective(float fov_y, float aspect, float near, float far);
CeMat4 ce_mat4_look_at(CeVec3 eye, CeVec3 target, CeVec3 up);
CeMat4 ce_mat4_translation(CeVec3 pos);
CeMat4 ce_mat4_rotation(CeQuat q);
CeMat4 ce_mat4_scale(CeVec3 scale);
CeMat4 ce_mat4_mul(CeMat4 a, CeMat4 b);
CeMat4 ce_mat4_trs(CeVec3 pos, CeQuat rot, CeVec3 scale);

/* ---- 四元数 ---- */
CeQuat ce_quat_identity(void);
CeQuat ce_quat_euler(float pitch, float yaw, float roll);

/* ---- 颜色 ---- */
CeColor ce_color_rgba(float r, float g, float b, float a);
CeColor ce_color_white(void);
CeColor ce_color_black(void);
CeColor ce_color_red(void);
CeColor ce_color_green(void);
CeColor ce_color_blue(void);

/* ---- 工具函数 ---- */
float ce_lerp(float a, float b, float t);
float ce_clamp(float v, float min, float max);
float ce_radians(float degrees);
float ce_degrees(float radians);

#endif /* CE_MATH_H */
