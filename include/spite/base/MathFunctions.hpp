#pragma once

#include "base/MathTypes.hpp"
#include "BaseExport.hpp"

namespace spite
{
    constexpr f32 PI = 3.14159265358979323846f;
    constexpr f32 TWOPI = PI * 2;

    SPITE_BASE_API f32 abs(f32 value);
    SPITE_BASE_API f32 min(f32 lhs, f32 rhs);
    SPITE_BASE_API f32 max(f32 lhs, f32 rhs);
    SPITE_BASE_API Vec2 min(const Vec2& lhs, const Vec2& rhs);
    SPITE_BASE_API Vec3 min(const Vec3& lhs, const Vec3& rhs);
    SPITE_BASE_API Vec4 min(const Vec4& lhs, const Vec4& rhs);
    SPITE_BASE_API Vec2 min(const Vec2& lhs, f32 rhs);
    SPITE_BASE_API Vec3 min(const Vec3& lhs, f32 rhs);
    SPITE_BASE_API Vec4 min(const Vec4& lhs, f32 rhs);
    SPITE_BASE_API Vec2 min(f32 lhs, const Vec2& rhs);
    SPITE_BASE_API Vec3 min(f32 lhs, const Vec3& rhs);
    SPITE_BASE_API Vec4 min(f32 lhs, const Vec4& rhs);
    SPITE_BASE_API Vec2 max(const Vec2& lhs, const Vec2& rhs);
    SPITE_BASE_API Vec3 max(const Vec3& lhs, const Vec3& rhs);
    SPITE_BASE_API Vec4 max(const Vec4& lhs, const Vec4& rhs);
    SPITE_BASE_API Vec2 max(const Vec2& lhs, f32 rhs);
    SPITE_BASE_API Vec3 max(const Vec3& lhs, f32 rhs);
    SPITE_BASE_API Vec4 max(const Vec4& lhs, f32 rhs);
    SPITE_BASE_API Vec2 max(f32 lhs, const Vec2& rhs);
    SPITE_BASE_API Vec3 max(f32 lhs, const Vec3& rhs);
    SPITE_BASE_API Vec4 max(f32 lhs, const Vec4& rhs);
    SPITE_BASE_API f32 lerp(f32 start, f32 end, f32 t);

    SPITE_BASE_API f32 toRadians(f32 degrees);
    SPITE_BASE_API f32 toDegrees(f32 radians);
    SPITE_BASE_API f32 clamp(f32 value, f32 minimum, f32 maximum);

    SPITE_BASE_API bool nearlyEqual(f32 lhs, f32 rhs, f32 epsilon = 1e-6f);

    SPITE_BASE_API f32 dot(const Vec2& lhs, const Vec2& rhs);
    SPITE_BASE_API f32 dot(const Vec3& lhs, const Vec3& rhs);
    SPITE_BASE_API f32 dot(const Vec4& lhs, const Vec4& rhs);

    SPITE_BASE_API Vec3 cross(const Vec3& lhs, const Vec3& rhs);
    SPITE_BASE_API Vec3 lerp(const Vec3& start, const Vec3& end, f32 t);
    SPITE_BASE_API Vec4 lerp(const Vec4& start, const Vec4& end, f32 t);

    SPITE_BASE_API f32 lengthSquared(const Vec2& value);
    SPITE_BASE_API f32 lengthSquared(const Vec3& value);
    SPITE_BASE_API f32 lengthSquared(const Vec4& value);

    SPITE_BASE_API f32 length(const Vec2& value);
    SPITE_BASE_API f32 length(const Vec3& value);
    SPITE_BASE_API f32 length(const Vec4& value);

    SPITE_BASE_API Vec2 normalize(const Vec2& value);
    SPITE_BASE_API Vec3 normalize(const Vec3& value);
    SPITE_BASE_API Vec4 normalize(const Vec4& value);

    SPITE_BASE_API Quat normalize(const Quat& value);
    SPITE_BASE_API Quat conjugate(const Quat& value);
    SPITE_BASE_API Quat inverse(const Quat& value);
    SPITE_BASE_API Quat multiply(const Quat& lhs, const Quat& rhs);
    SPITE_BASE_API f32 dot(const Quat& lhs, const Quat& rhs);
    SPITE_BASE_API Quat slerp(const Quat& start, const Quat& end, f32 t);
    SPITE_BASE_API Vec3 quatToEulerAnglesZyx(const Quat& value);
    SPITE_BASE_API Quat eulerAnglesZyxToQuat(const Vec3& eulerAnglesRadians);
    SPITE_BASE_API Vec3 rotate(const Quat& rotation, const Vec3& vector);
    SPITE_BASE_API Quat fromAxisAngle(const Vec3& axis, f32 angleRadians);
    SPITE_BASE_API f32 quatLength(const Quat& value);

    SPITE_BASE_API Mat3 transpose(const Mat3& value);
    SPITE_BASE_API Mat4 transpose(const Mat4& value);

    SPITE_BASE_API Mat4 multiply(const Mat4& lhs, const Mat4& rhs);
    SPITE_BASE_API Vec3 multiply(const Mat3& lhs, const Vec3& rhs);
    SPITE_BASE_API Vec4 multiply(const Mat4& lhs, const Vec4& rhs);
    SPITE_BASE_API Vec3 multiplyPoint(const Mat4& lhs, const Vec3& rhs);
    SPITE_BASE_API Vec3 multiplyVector(const Mat4& lhs, const Vec3& rhs);

    SPITE_BASE_API Mat4 translate(const Vec3& value);
    SPITE_BASE_API Mat4 scale(const Vec3& value);
    SPITE_BASE_API Mat4 rotate(const Quat& value);
    SPITE_BASE_API Mat4 trs(const Vec3& translation, const Quat& rotation, const Vec3& scaleValue);

    SPITE_BASE_API f32 determinant(const Mat4& value);
    SPITE_BASE_API Mat4 inverse(const Mat4& value);
    SPITE_BASE_API Mat4 inverseTranspose(const Mat4& value);

    SPITE_BASE_API Mat4 perspectiveRhZo(f32 fovYRadians, f32 aspectRatio, f32 nearPlane, f32 farPlane);
    SPITE_BASE_API Mat4 perspectiveRhZoReverseZ(f32 fovYRadians, f32 aspectRatio, f32 nearPlane, f32 farPlane);
    SPITE_BASE_API Mat4 perspectiveRhZoReverseZInfinite(f32 fovYRadians, f32 aspectRatio, f32 nearPlane);
    SPITE_BASE_API Mat4 lookAtRh(const Vec3& eye, const Vec3& target, const Vec3& up);

    SPITE_BASE_API u32 packUnorm2x16(const Vec2& value);
    SPITE_BASE_API Vec2 unpackUnorm2x16(u32 packedValue);
    SPITE_BASE_API u32 packUnorm4x8(const Vec4& value);
    SPITE_BASE_API Vec4 unpackUnorm4x8(u32 packedValue);

    SPITE_FINLINE Mat4 operator*(const Mat4& lhs, const Mat4& rhs) { return multiply(lhs, rhs); }
    SPITE_FINLINE Vec3 operator*(const Mat3& lhs, const Vec3& rhs) { return multiply(lhs, rhs); }
    SPITE_FINLINE Vec4 operator*(const Mat4& lhs, const Vec4& rhs) { return multiply(lhs, rhs); }
    SPITE_FINLINE Quat operator*(const Quat& lhs, const Quat& rhs) { return multiply(lhs, rhs); }
} // namespace spite
