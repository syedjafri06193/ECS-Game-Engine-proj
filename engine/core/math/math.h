#pragma once
// Minimal math: what the ECS, extraction and clustering need. Column-major
// matrices, right-handed view space looking down -Z, Vulkan clip space
// (depth 0..1, +Y down handled in the projection).
#include <cmath>

namespace engine {

struct Vec2 { float x = 0, y = 0; };
struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
};
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Mat4 { float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; };   // column-major

constexpr float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3  Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float    Length(Vec3 v) { return std::sqrt(Dot(v, v)); }
inline Vec3     Normalize(Vec3 v) { float l = Length(v); return l > 0 ? v * (1.0f / l) : v; }
constexpr float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline Quat QuatAxisAngle(Vec3 axis, float radians)
{
    Vec3 a = Normalize(axis);
    float s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
}

inline Quat operator*(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Vec3 Rotate(Quat q, Vec3 v)
{
    Vec3 u{q.x, q.y, q.z};
    Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

inline Vec4 operator*(const Mat4& a, Vec4 v)
{
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
            a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w};
}

inline Vec3 TransformPoint(const Mat4& a, Vec3 p)
{
    Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

// Translation * Rotation * Scale
inline Mat4 ComposeTRS(Vec3 t, Quat q, Vec3 s)
{
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m;
    m.m[0] = (1 - 2 * (yy + zz)) * s.x; m.m[1] = 2 * (xy + wz) * s.x;       m.m[2] = 2 * (xz - wy) * s.x;       m.m[3] = 0;
    m.m[4] = 2 * (xy - wz) * s.y;       m.m[5] = (1 - 2 * (xx + zz)) * s.y; m.m[6] = 2 * (yz + wx) * s.y;       m.m[7] = 0;
    m.m[8] = 2 * (xz + wy) * s.z;       m.m[9] = 2 * (yz - wx) * s.z;       m.m[10] = (1 - 2 * (xx + yy)) * s.z; m.m[11] = 0;
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z; m.m[15] = 1;
    return m;
}

inline Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    Vec3 f = Normalize(target - eye);
    Vec3 s = Normalize(Cross(f, up));
    Vec3 u = Cross(s, f);
    Mat4 m;
    m.m[0] = s.x; m.m[4] = s.y; m.m[8] = s.z;
    m.m[1] = u.x; m.m[5] = u.y; m.m[9] = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -Dot(s, eye); m.m[13] = -Dot(u, eye); m.m[14] = Dot(f, eye);
    return m;
}

// Vulkan: depth 0..1, Y flipped.
inline Mat4 Perspective(float fovy_radians, float aspect, float znear, float zfar)
{
    const float f = 1.0f / std::tan(fovy_radians * 0.5f);
    Mat4 m;
    for (float& v : m.m) v = 0;
    m.m[0] = f / aspect;
    m.m[5] = -f;
    m.m[10] = zfar / (znear - zfar);
    m.m[11] = -1.0f;
    m.m[14] = (znear * zfar) / (znear - zfar);
    return m;
}

constexpr float kPi = 3.14159265358979323846f;

}  // namespace engine
