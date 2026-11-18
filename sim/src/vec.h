#pragma once
#include <cmath>

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(double s, Vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { return a = a - b; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double norm(Vec3 a) { return std::sqrt(dot(a, a)); }

struct Mat3 {
    double m[3][3] = {};
};

inline Mat3 identity(double s) {
    Mat3 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}

inline Mat3 outer(Vec3 a, Vec3 b) {
    double u[3] = {a.x, a.y, a.z}, v[3] = {b.x, b.y, b.z};
    Mat3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = u[i] * v[j];
    return r;
}

inline Mat3 operator+(Mat3 a, const Mat3& b) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) a.m[i][j] += b.m[i][j];
    return a;
}

inline Mat3 operator*(double s, Mat3 a) {
    for (auto& row : a.m)
        for (double& e : row) e *= s;
    return a;
}

inline Mat3 operator-(Mat3 a, const Mat3& b) { return a + (-1.0) * b; }

inline double det(Vec3 c0, Vec3 c1, Vec3 c2) { return dot(c0, {c1.y * c2.z - c1.z * c2.y, c1.z * c2.x - c1.x * c2.z, c1.x * c2.y - c1.y * c2.x}); }

// Solves A x = b with Cramer's rule.
inline Vec3 solve(const Mat3& A, Vec3 b) {
    Vec3 c0{A.m[0][0], A.m[1][0], A.m[2][0]};
    Vec3 c1{A.m[0][1], A.m[1][1], A.m[2][1]};
    Vec3 c2{A.m[0][2], A.m[1][2], A.m[2][2]};
    double d = det(c0, c1, c2);
    if (std::abs(d) < 1e-20) return {};
    return {det(b, c1, c2) / d, det(c0, b, c2) / d, det(c0, c1, b) / d};
}
