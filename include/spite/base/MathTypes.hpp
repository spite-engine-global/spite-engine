#pragma once

#include <type_traits>

#include "base/Platform.hpp"
#include "BaseExport.hpp"

namespace spite
{
    struct Vec2;
    struct Vec3;
    struct Vec4;
    struct Quat;
    struct Mat3;
    struct Mat4;

    struct SPITE_BASE_API Vec2
    {
        f32 x = 0.0f;
        f32 y = 0.0f;

        constexpr Vec2() = default;
        constexpr Vec2(f32 xValue, f32 yValue) : x(xValue), y(yValue) {}
        explicit constexpr Vec2(f32 value) : x(value), y(value) {}
        constexpr Vec2(const Vec3& value);
        constexpr Vec2(const Vec4& value);

        constexpr f32& operator[](u32 index) { return index == 0 ? x : y; }
        constexpr const f32& operator[](u32 index) const { return index == 0 ? x : y; }
        constexpr Vec2& operator+=(const Vec2& rhs)
        {
            x += rhs.x;
            y += rhs.y;
            return *this;
        }
        constexpr Vec2& operator-=(const Vec2& rhs)
        {
            x -= rhs.x;
            y -= rhs.y;
            return *this;
        }
        constexpr Vec2& operator*=(f32 scalar)
        {
            x *= scalar;
            y *= scalar;
            return *this;
        }
        constexpr Vec2& operator*=(const Vec2& rhs)
        {
            x *= rhs.x;
            y *= rhs.y;
            return *this;
        }
        constexpr Vec2& operator/=(f32 scalar)
        {
            x /= scalar;
            y /= scalar;
            return *this;
        }

        constexpr f32* data() { return &x; }
        constexpr const f32* data() const { return &x; }
    };

    struct SPITE_BASE_API Vec3
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;

        constexpr Vec3() = default;
        constexpr Vec3(f32 xValue, f32 yValue, f32 zValue) : x(xValue), y(yValue), z(zValue) {}
        explicit constexpr Vec3(f32 value) : x(value), y(value), z(value) {}
        constexpr Vec3(const Vec2& xy, f32 zValue) : x(xy.x), y(xy.y), z(zValue) {}
        constexpr Vec3(const Vec4& value);

        constexpr f32& operator[](u32 index)
        {
            if (index == 0)
            {
                return x;
            }
            return index == 1 ? y : z;
        }

        constexpr const f32& operator[](u32 index) const
        {
            if (index == 0)
            {
                return x;
            }
            return index == 1 ? y : z;
        }
        constexpr Vec3& operator+=(const Vec3& rhs)
        {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            return *this;
        }
        constexpr Vec3& operator-=(const Vec3& rhs)
        {
            x -= rhs.x;
            y -= rhs.y;
            z -= rhs.z;
            return *this;
        }
        constexpr Vec3& operator*=(f32 scalar)
        {
            x *= scalar;
            y *= scalar;
            z *= scalar;
            return *this;
        }
        constexpr Vec3& operator*=(const Vec3& rhs)
        {
            x *= rhs.x;
            y *= rhs.y;
            z *= rhs.z;
            return *this;
        }
        constexpr Vec3& operator/=(f32 scalar)
        {
            x /= scalar;
            y /= scalar;
            z /= scalar;
            return *this;
        }

        constexpr f32* data() { return &x; }
        constexpr const f32* data() const { return &x; }
    };

    struct SPITE_BASE_API Vec4
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        f32 w = 0.0f;

        constexpr Vec4() = default;
        constexpr Vec4(f32 xValue, f32 yValue, f32 zValue, f32 wValue) : x(xValue), y(yValue), z(zValue), w(wValue) {}
        explicit constexpr Vec4(f32 value) : x(value), y(value), z(value), w(value) {}
        constexpr Vec4(const Vec2& value, f32 zValue, f32 wValue) : x(value.x), y(value.y), z(zValue), w(wValue) {}
        constexpr Vec4(const Vec3& xyz, f32 wValue) : x(xyz.x), y(xyz.y), z(xyz.z), w(wValue) {}

        constexpr f32& operator[](u32 index)
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            return index == 2 ? z : w;
        }

        constexpr const f32& operator[](u32 index) const
        {
            if (index == 0)
            {
                return x;
            }
            if (index == 1)
            {
                return y;
            }
            return index == 2 ? z : w;
        }
        constexpr Vec4& operator+=(const Vec4& rhs)
        {
            x += rhs.x;
            y += rhs.y;
            z += rhs.z;
            w += rhs.w;
            return *this;
        }
        constexpr Vec4& operator-=(const Vec4& rhs)
        {
            x -= rhs.x;
            y -= rhs.y;
            z -= rhs.z;
            w -= rhs.w;
            return *this;
        }
        constexpr Vec4& operator*=(f32 scalar)
        {
            x *= scalar;
            y *= scalar;
            z *= scalar;
            w *= scalar;
            return *this;
        }
        constexpr Vec4& operator*=(const Vec4& rhs)
        {
            x *= rhs.x;
            y *= rhs.y;
            z *= rhs.z;
            w *= rhs.w;
            return *this;
        }
        constexpr Vec4& operator/=(f32 scalar)
        {
            x /= scalar;
            y /= scalar;
            z /= scalar;
            w /= scalar;
            return *this;
        }

        constexpr f32* data() { return &x; }
        constexpr const f32* data() const { return &x; }
    };

    struct SPITE_BASE_API Quat
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        f32 w = 1.0f;

        constexpr Quat() = default;
        constexpr Quat(f32 xValue, f32 yValue, f32 zValue, f32 wValue) : x(xValue), y(yValue), z(zValue), w(wValue) {}
        constexpr Quat(const Vec4& value) : x(value.x), y(value.y), z(value.z), w(value.w) {}

        static constexpr Quat identity() { return Quat(0.0f, 0.0f, 0.0f, 1.0f); }
        constexpr Quat& operator*=(f32 scalar)
        {
            x *= scalar;
            y *= scalar;
            z *= scalar;
            w *= scalar;
            return *this;
        }
        constexpr Quat& operator/=(f32 scalar)
        {
            x /= scalar;
            y /= scalar;
            z /= scalar;
            w /= scalar;
            return *this;
        }

        constexpr f32* data() { return &x; }
        constexpr const f32* data() const { return &x; }
    };

    struct SPITE_BASE_API Mat3
    {
        struct ColumnProxy
        {
            f32* m_columnData = nullptr;

            constexpr f32& operator[](u32 row) { return m_columnData[row]; }
            constexpr const f32& operator[](u32 row) const { return m_columnData[row]; }
            constexpr operator Vec3() const { return Vec3(m_columnData[0], m_columnData[1], m_columnData[2]); }
            constexpr ColumnProxy& operator=(const Vec3& value)
            {
                m_columnData[0] = value.x;
                m_columnData[1] = value.y;
                m_columnData[2] = value.z;
                return *this;
            }
        };

        struct ConstColumnProxy
        {
            const f32* m_columnData = nullptr;

            constexpr const f32& operator[](u32 row) const { return m_columnData[row]; }
            constexpr operator Vec3() const { return Vec3(m_columnData[0], m_columnData[1], m_columnData[2]); }
        };

        f32 elements[9]{};

        constexpr Mat3() = default;

        explicit constexpr Mat3(f32 diagonal)
        {
            elements[0] = diagonal;
            elements[4] = diagonal;
            elements[8] = diagonal;
        }
        explicit constexpr Mat3(const Mat4& value);
        explicit constexpr Mat3(const Quat& value);

        static constexpr Mat3 identity() { return Mat3(1.0f); }

        constexpr f32& operator()(u32 column, u32 row) { return elements[column * 3u + row]; }
        constexpr const f32& operator()(u32 column, u32 row) const { return elements[column * 3u + row]; }
        constexpr ColumnProxy operator[](u32 column) { return ColumnProxy{&elements[column * 3u]}; }
        constexpr ConstColumnProxy operator[](u32 column) const { return ConstColumnProxy{&elements[column * 3u]}; }
        constexpr Mat3& operator*=(f32 scalar)
        {
            for (u32 i = 0; i < 9u; ++i)
            {
                elements[i] *= scalar;
            }
            return *this;
        }
        constexpr Mat3& operator/=(f32 scalar)
        {
            for (u32 i = 0; i < 9u; ++i)
            {
                elements[i] /= scalar;
            }
            return *this;
        }

        constexpr f32* data() { return elements; }
        constexpr const f32* data() const { return elements; }
    };

    struct SPITE_BASE_API Mat4
    {
        struct ColumnProxy
        {
            f32* m_columnData = nullptr;

            constexpr f32& operator[](u32 row) { return m_columnData[row]; }
            constexpr const f32& operator[](u32 row) const { return m_columnData[row]; }
            constexpr operator Vec4() const
            {
                return Vec4(m_columnData[0], m_columnData[1], m_columnData[2], m_columnData[3]);
            }
            constexpr ColumnProxy& operator=(const Vec4& value)
            {
                m_columnData[0] = value.x;
                m_columnData[1] = value.y;
                m_columnData[2] = value.z;
                m_columnData[3] = value.w;
                return *this;
            }
        };

        struct ConstColumnProxy
        {
            const f32* m_columnData = nullptr;

            constexpr const f32& operator[](u32 row) const { return m_columnData[row]; }
            constexpr operator Vec4() const
            {
                return Vec4(m_columnData[0], m_columnData[1], m_columnData[2], m_columnData[3]);
            }
        };

        f32 elements[16]{};

        constexpr Mat4() = default;

        explicit constexpr Mat4(f32 diagonal)
        {
            elements[0] = diagonal;
            elements[5] = diagonal;
            elements[10] = diagonal;
            elements[15] = diagonal;
        }
        explicit constexpr Mat4(const Mat3& value);
        explicit constexpr Mat4(const Quat& value);
        constexpr Mat4(const Vec4& column0, const Vec4& column1, const Vec4& column2, const Vec4& column3) :
            elements{column0.x, column0.y, column0.z, column0.w, column1.x, column1.y, column1.z, column1.w,
                     column2.x, column2.y, column2.z, column2.w, column3.x, column3.y, column3.z, column3.w}
        {
        }

        static constexpr Mat4 identity() { return Mat4(1.0f); }

        constexpr f32& operator()(u32 column, u32 row) { return elements[column * 4u + row]; }
        constexpr const f32& operator()(u32 column, u32 row) const { return elements[column * 4u + row]; }
        constexpr ColumnProxy operator[](u32 column) { return ColumnProxy{&elements[column * 4u]}; }
        constexpr ConstColumnProxy operator[](u32 column) const { return ConstColumnProxy{&elements[column * 4u]}; }
        constexpr Vec4 column(u32 index) const
        {
            return Vec4(elements[index * 4u + 0u], elements[index * 4u + 1u], elements[index * 4u + 2u],
                        elements[index * 4u + 3u]);
        }
        constexpr Mat4& operator*=(f32 scalar)
        {
            for (u32 i = 0; i < 16u; ++i)
            {
                elements[i] *= scalar;
            }
            return *this;
        }
        constexpr Mat4& operator/=(f32 scalar)
        {
            for (u32 i = 0; i < 16u; ++i)
            {
                elements[i] /= scalar;
            }
            return *this;
        }

        constexpr f32* data() { return elements; }
        constexpr const f32* data() const { return elements; }
    };

    constexpr Vec2::Vec2(const Vec3& value) : x(value.x), y(value.y) {}
    constexpr Vec2::Vec2(const Vec4& value) : x(value.x), y(value.y) {}
    constexpr Vec3::Vec3(const Vec4& value) : x(value.x), y(value.y), z(value.z) {}

    constexpr Mat3::Mat3(const Mat4& value)
    {
        elements[0] = value(0, 0);
        elements[1] = value(0, 1);
        elements[2] = value(0, 2);
        elements[3] = value(1, 0);
        elements[4] = value(1, 1);
        elements[5] = value(1, 2);
        elements[6] = value(2, 0);
        elements[7] = value(2, 1);
        elements[8] = value(2, 2);
    }

    constexpr Mat3::Mat3(const Quat& value)
    {
        const f32 xx = value.x * value.x;
        const f32 yy = value.y * value.y;
        const f32 zz = value.z * value.z;
        const f32 xy = value.x * value.y;
        const f32 xz = value.x * value.z;
        const f32 yz = value.y * value.z;
        const f32 wx = value.w * value.x;
        const f32 wy = value.w * value.y;
        const f32 wz = value.w * value.z;

        elements[0] = 1.0f - 2.0f * (yy + zz);
        elements[1] = 2.0f * (xy + wz);
        elements[2] = 2.0f * (xz - wy);

        elements[3] = 2.0f * (xy - wz);
        elements[4] = 1.0f - 2.0f * (xx + zz);
        elements[5] = 2.0f * (yz + wx);

        elements[6] = 2.0f * (xz + wy);
        elements[7] = 2.0f * (yz - wx);
        elements[8] = 1.0f - 2.0f * (xx + yy);
    }

    constexpr Mat4::Mat4(const Mat3& value)
    {
        elements[0] = value(0, 0);
        elements[1] = value(0, 1);
        elements[2] = value(0, 2);
        elements[4] = value(1, 0);
        elements[5] = value(1, 1);
        elements[6] = value(1, 2);
        elements[8] = value(2, 0);
        elements[9] = value(2, 1);
        elements[10] = value(2, 2);
        elements[15] = 1.0f;
    }

    constexpr Mat4::Mat4(const Quat& value) : Mat4(Mat3(value)) {}

    static_assert(std::is_trivially_copyable_v<Vec2>);
    static_assert(std::is_standard_layout_v<Vec2>);
    static_assert(sizeof(Vec2) == sizeof(f32) * 2u);

    static_assert(std::is_trivially_copyable_v<Vec3>);
    static_assert(std::is_standard_layout_v<Vec3>);
    static_assert(sizeof(Vec3) == sizeof(f32) * 3u);

    static_assert(std::is_trivially_copyable_v<Vec4>);
    static_assert(std::is_standard_layout_v<Vec4>);
    static_assert(sizeof(Vec4) == sizeof(f32) * 4u);

    static_assert(std::is_trivially_copyable_v<Quat>);
    static_assert(std::is_standard_layout_v<Quat>);
    static_assert(sizeof(Quat) == sizeof(f32) * 4u);

    static_assert(std::is_trivially_copyable_v<Mat3>);
    static_assert(std::is_standard_layout_v<Mat3>);
    static_assert(sizeof(Mat3) == sizeof(f32) * 9u);

    static_assert(std::is_trivially_copyable_v<Mat4>);
    static_assert(std::is_standard_layout_v<Mat4>);
    static_assert(sizeof(Mat4) == sizeof(f32) * 16u);

    SPITE_FINLINE constexpr Vec2 operator+(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x + rhs.x, lhs.y + rhs.y);
    }
    SPITE_FINLINE constexpr Vec2 operator-(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x - rhs.x, lhs.y - rhs.y);
    }
    SPITE_FINLINE constexpr Vec2 operator-(const Vec2& value) { return Vec2(-value.x, -value.y); }
    SPITE_FINLINE constexpr Vec2 operator*(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x * rhs.x, lhs.y * rhs.y);
    }
    SPITE_FINLINE constexpr Vec2 operator*(const Vec2& value, f32 scalar)
    {
        return Vec2(value.x * scalar, value.y * scalar);
    }
    SPITE_FINLINE constexpr Vec2 operator*(f32 scalar, const Vec2& value) { return value * scalar; }
    SPITE_FINLINE constexpr Vec2 operator/(const Vec2& value, f32 scalar)
    {
        return Vec2(value.x / scalar, value.y / scalar);
    }
    SPITE_FINLINE constexpr Vec2 operator/(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x / rhs.x, lhs.y / rhs.y);
    }

    SPITE_FINLINE constexpr Vec3 operator+(const Vec3& lhs, const Vec3& rhs)
    {
        return Vec3(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
    }
    SPITE_FINLINE constexpr Vec3 operator-(const Vec3& lhs, const Vec3& rhs)
    {
        return Vec3(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z);
    }
    SPITE_FINLINE constexpr Vec3 operator-(const Vec3& value) { return Vec3(-value.x, -value.y, -value.z); }
    SPITE_FINLINE constexpr Vec3 operator*(const Vec3& lhs, const Vec3& rhs)
    {
        return Vec3(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z);
    }
    SPITE_FINLINE constexpr Vec3 operator*(const Vec3& value, f32 scalar)
    {
        return Vec3(value.x * scalar, value.y * scalar, value.z * scalar);
    }
    SPITE_FINLINE constexpr Vec3 operator*(f32 scalar, const Vec3& value) { return value * scalar; }
    SPITE_FINLINE constexpr Vec3 operator/(const Vec3& value, f32 scalar)
    {
        return Vec3(value.x / scalar, value.y / scalar, value.z / scalar);
    }
    SPITE_FINLINE constexpr Vec3 operator/(const Vec3& lhs, const Vec3& rhs)
    {
        return Vec3(lhs.x / rhs.x, lhs.y / rhs.y, lhs.z / rhs.z);
    }

    SPITE_FINLINE constexpr Vec4 operator+(const Vec4& lhs, const Vec4& rhs)
    {
        return Vec4(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w);
    }

    SPITE_FINLINE constexpr Vec4 operator-(const Vec4& lhs, const Vec4& rhs)
    {
        return Vec4(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w);
    }
    SPITE_FINLINE constexpr Vec4 operator-(const Vec4& value) { return Vec4(-value.x, -value.y, -value.z, -value.w); }
    SPITE_FINLINE constexpr Vec4 operator*(const Vec4& lhs, const Vec4& rhs)
    {
        return Vec4(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z, lhs.w * rhs.w);
    }

    SPITE_FINLINE constexpr Vec4 operator*(const Vec4& value, f32 scalar)
    {
        return Vec4(value.x * scalar, value.y * scalar, value.z * scalar, value.w * scalar);
    }

    SPITE_FINLINE constexpr Vec4 operator*(f32 scalar, const Vec4& value) { return value * scalar; }
    SPITE_FINLINE constexpr Vec4 operator/(const Vec4& value, f32 scalar)
    {
        return Vec4(value.x / scalar, value.y / scalar, value.z / scalar, value.w / scalar);
    }
    SPITE_FINLINE constexpr Vec4 operator/(const Vec4& lhs, const Vec4& rhs)
    {
        return Vec4(lhs.x / rhs.x, lhs.y / rhs.y, lhs.z / rhs.z, lhs.w / rhs.w);
    }
    SPITE_FINLINE constexpr Quat operator/(const Quat& value, f32 scalar)
    {
        return Quat(value.x / scalar, value.y / scalar, value.z / scalar, value.w / scalar);
    }
    SPITE_FINLINE constexpr Mat3 operator/(const Mat3& value, f32 scalar)
    {
        Mat3 result = value;
        result /= scalar;
        return result;
    }
    SPITE_FINLINE constexpr Mat4 operator/(const Mat4& value, f32 scalar)
    {
        Mat4 result = value;
        result /= scalar;
        return result;
    }

    SPITE_FINLINE constexpr bool operator==(const Vec2& lhs, const Vec2& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y;
    }
    SPITE_FINLINE constexpr bool operator==(const Vec3& lhs, const Vec3& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
    SPITE_FINLINE constexpr bool operator==(const Vec4& lhs, const Vec4& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
    }
    SPITE_FINLINE constexpr bool operator==(const Quat& lhs, const Quat& rhs)
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
    }
    SPITE_FINLINE constexpr bool operator!=(const Vec2& lhs, const Vec2& rhs) { return !(lhs == rhs); }
    SPITE_FINLINE constexpr bool operator!=(const Vec3& lhs, const Vec3& rhs) { return !(lhs == rhs); }
    SPITE_FINLINE constexpr bool operator!=(const Vec4& lhs, const Vec4& rhs) { return !(lhs == rhs); }
    SPITE_FINLINE constexpr bool operator!=(const Quat& lhs, const Quat& rhs) { return !(lhs == rhs); }

    SPITE_FINLINE constexpr bool operator==(const Mat3& lhs, const Mat3& rhs)
    {
        for (u32 i = 0; i < 9u; ++i)
        {
            if (lhs.elements[i] != rhs.elements[i])
            {
                return false;
            }
        }
        return true;
    }

    SPITE_FINLINE constexpr bool operator!=(const Mat3& lhs, const Mat3& rhs) { return !(lhs == rhs); }

    SPITE_FINLINE constexpr bool operator==(const Mat4& lhs, const Mat4& rhs)
    {
        for (u32 i = 0; i < 16u; ++i)
        {
            if (lhs.elements[i] != rhs.elements[i])
            {
                return false;
            }
        }
        return true;
    }

    SPITE_FINLINE constexpr bool operator!=(const Mat4& lhs, const Mat4& rhs) { return !(lhs == rhs); }

    struct BoundingBox
    {
        Vec3 min{0.f};
        Vec3 max{0.f};
    };
} // namespace spite
