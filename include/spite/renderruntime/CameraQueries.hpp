#pragma once

#include <cmath>

#include "base/MathFunctions.hpp"
#include "ecs/World.hpp"
#include "renderruntime/RenderView.hpp"
#include "renderruntime/RenderingComponents.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    // Stateless camera math over the world: one definition of view/projection
    // per camera entity and aspect, shared by the upload system and host-side
    // tooling. The upload system stays the only writer of the computed
    // matrices; these helpers write components/bindings and read the last
    // frame's matrices.

    // Reverse-Z orthographic projection of a box `height` tall (and `height * aspect` wide) from nearPlane to farPlane
    // in front of the eye; element order as perspectiveRhZoReverseZ.
    inline Mat4 orthographicRhZoReverseZ(const float height, const float aspect, const float nearPlane,
                                         const float farPlane)
    {
        Mat4 result{};
        result(0, 0) = 2.0f / (height * aspect);
        result(1, 1) = 2.0f / height;
        result(2, 2) = 1.0f / (farPlane - nearPlane);
        result(3, 2) = farPlane / (farPlane - nearPlane);
        result(3, 3) = 1.0f;
        return result;
    }

    // orthographicHeight > 0: the projection is orthographic (CameraPose::orthographicHeight)
    inline CameraMatricesData computeCameraMatrices(const TransformComponent& transform, const CameraComponent& camera,
                                                    const float aspect, const float orthographicHeight = 0.0f)
    {
        CameraMatricesData out{};
        out.view = inverse(translate(transform.position) * Mat4(transform.rotation));
        out.pos = transform.position;
        out.projection = orthographicHeight > 0.0f
            ? orthographicRhZoReverseZ(orthographicHeight, aspect, camera.nearPlane, camera.farPlane)
            : perspectiveRhZoReverseZ(camera.fov, aspect, camera.nearPlane, camera.farPlane);
        out.viewProjection = out.projection * out.view;
        return out;
    }

    // The six clip planes of a ZO view-projection (left, right, bottom, top, z >= 0, z <= w), normalised: xyz is
    // the inward normal, w the distance term.
    inline void extractFrustumPlanes(const Mat4& vp, Vec4 (&planes)[6])
    {
        const auto row = [&vp](const u32 i) { return Vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
        planes[0] = row(3) + row(0);
        planes[1] = row(3) - row(0);
        planes[2] = row(3) + row(1);
        planes[3] = row(3) - row(1);
        planes[4] = row(2);
        planes[5] = row(3) - row(2);
        for (Vec4& plane : planes)
        {
            const float planeLength = length(Vec3(plane));
            if (planeLength > 1e-6f)
            {
                plane /= planeLength;
            }
        }
    }

    // World-space view direction: the negated third row of the view matrix
    // (camera +Z points backward in the RH convention).
    inline Vec3 cameraForward(const CameraMatricesData& matrices)
    {
        return -normalize(Vec3(matrices.view[0][2], matrices.view[1][2], matrices.view[2][2]));
    }

    // Picking ray through the given NDC point of the viewport the matrices
    // were computed for. False when the point is degenerate.
    inline bool cameraRayFromNdc(const CameraMatricesData& matrices, const Vec2 ndc, Vec3& outOrigin,
                                 Vec3& outDirection)
    {
        outOrigin = matrices.pos;
        const Mat4 inverseViewProjection = inverse(matrices.viewProjection);
        const Vec4 clip = inverseViewProjection * Vec4(ndc.x, ndc.y, 1.0f, 1.0f);
        if (std::abs(clip.w) < 1e-8f)
        {
            return false;
        }
        const Vec3 world = Vec3(clip) / clip.w;
        outDirection = normalize(world - matrices.pos);
        return dot(outDirection, outDirection) > 1e-10f;
    }

    // Returns the view's camera entity, creating and binding a default one
    // when the binding is missing or no longer a camera. Call on the host
    // thread outside the frame's systems.
    inline Entity ensureCamera(World& world, RenderView& view)
    {
        Entity& bound = view.camera;
        if (world.isEntityValid(bound) && world.isEntityRegistered(bound) &&
            world.hasComponent<TransformComponent>(bound) && world.hasComponent<CameraComponent>(bound))
        {
            return bound;
        }

        const Entity camera = world.createEntity();
        world.addComponent<TransformComponent>(camera, TransformComponent{.position = {0.0f, 2.0f, 15.0f}});
        world.addComponent<TransformMatrixComponent>(camera);
        world.addComponent<CameraComponent>(camera);
        bound = camera;
        return camera;
    }

    // The rotation that points a camera's -Z from position at target.
    inline Quat lookAtRotation(const Vec3& position, const Vec3& target, const Vec3& up = Vec3(0.f, 1.f, 0.f))
    {
        const Vec3 backward = normalize(position - target);
        const Vec3 right = normalize(cross(up, backward));
        const Vec3 newUp = cross(backward, right);

        // rotation matrix columns = camera axes in world space; quaternion via
        // the largest-diagonal branch of the standard conversion (rotation is
        // column-major: rotation[column][row])
        const Mat4 rotation(Vec4(right, 0.f), Vec4(newUp, 0.f), Vec4(backward, 0.f), Vec4(0.f, 0.f, 0.f, 1.f));
        const f32 trace = rotation[0][0] + rotation[1][1] + rotation[2][2];
        Quat quat;
        if (trace > 0.f)
        {
            const f32 s = std::sqrt(trace + 1.f) * 2.f;
            quat = Quat((rotation[1][2] - rotation[2][1]) / s, (rotation[2][0] - rotation[0][2]) / s,
                        (rotation[0][1] - rotation[1][0]) / s, s / 4.f);
        }
        else if (rotation[0][0] > rotation[1][1] && rotation[0][0] > rotation[2][2])
        {
            const f32 s = std::sqrt(1.f + rotation[0][0] - rotation[1][1] - rotation[2][2]) * 2.f;
            quat = Quat(s / 4.f, (rotation[0][1] + rotation[1][0]) / s, (rotation[0][2] + rotation[2][0]) / s,
                        (rotation[1][2] - rotation[2][1]) / s);
        }
        else if (rotation[1][1] > rotation[2][2])
        {
            const f32 s = std::sqrt(1.f + rotation[1][1] - rotation[0][0] - rotation[2][2]) * 2.f;
            quat = Quat((rotation[0][1] + rotation[1][0]) / s, s / 4.f, (rotation[1][2] + rotation[2][1]) / s,
                        (rotation[2][0] - rotation[0][2]) / s);
        }
        else
        {
            const f32 s = std::sqrt(1.f + rotation[2][2] - rotation[0][0] - rotation[1][1]) * 2.f;
            quat = Quat((rotation[0][2] + rotation[2][0]) / s, (rotation[1][2] + rotation[2][1]) / s, s / 4.f,
                        (rotation[0][1] - rotation[1][0]) / s);
        }
        return quat;
    }

    // Points the camera's -Z at target by writing the entity transform's
    // rotation.
    inline void cameraLookAt(World& world, const Entity camera, const Vec3& target,
                             const Vec3& up = Vec3(0.f, 1.f, 0.f))
    {
        auto& transform = world.getComponent<TransformComponent>(camera);
        transform.rotation = lookAtRotation(transform.position, target, up);
    }
} // namespace spite
