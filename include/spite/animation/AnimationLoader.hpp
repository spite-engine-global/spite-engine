#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // One joint of a skeleton in plain data, as the loader hands it over and AnimationWorld::joints returns it.
    struct JointInfo
    {
        InternedString name;
        i32 parent = -1; // index into the skeleton's joints, -1 for a root; always below the joint's own index
        Vec3 position{0.0f}; // local rest pose
        Quat rotation = Quat::identity();
        Vec3 scale{1.0f};
        Mat4 inverseBind = Mat4::identity(); // model space to the joint's bind space; identity for an unbound joint
    };

    // A baked skeleton: its joints and the ozz archive the module builds its own skeleton from. identity is the hash
    // of the joint names, parents and rest poses that the clips baked against it carry.
    struct SkeletonSource
    {
        InternedString name;
        u64 identity = 0;
        // the source's up axis is Z (ModelImportSettings::upAxis): its ground plane is x,y, otherwise x,z. The bake
        // holds that plane's travel of the root joints, and so does the pose.
        bool zUp = false;
        // the model's import settings usesLayers and usesIk (ModelImportSettings): its baked bounds hold the poses of
        // animation layers and of IK nodes only when set, so the runtime refuses what the model did not declare
        bool usesLayers = false;
        bool usesIk = false;
        std::vector<JointInfo> joints;
        std::vector<std::byte> archive;
    };

    // A baked clip: the ozz animation archive and what it was baked against.
    struct ClipSource
    {
        InternedString name;
        InternedString skeleton;
        u64 skeletonIdentity = 0;
        f32 duration = 0.0f;
        u32 trackCount = 0; // one per joint of the skeleton
        std::vector<std::byte> archive;
    };

    // How baked skeleton and clip bytes reach SpiteAnimation, which references no asset module: the host registers one
    // of these as a service of the root service locator (SpiteAssetRuntime does, over the baked loaders), and every
    // AnimationWorld below it reads it through its world's services. Both functions run on the frame thread and
    // return an error that names what failed (a missing or damaged file, a clip baked for another skeleton) or a
    // value that is the whole file; neither may touch the service locator.
    struct AnimationAssetLoader
    {
        std::function<Expected<SkeletonSource>(InternedString skeleton)> loadSkeleton;
        // the clip as it plays on skeleton: an error when it was baked for another one
        std::function<Expected<ClipSource>(InternedString clip, InternedString skeleton)> loadClip;
        // Which names an import or reimport has committed since a cursor: appends the model, skeleton and clip names
        // to names and returns the cursor to pass next time (0 the first time). Cheap when nothing was committed
        // (one comparison under a lock, nothing appended); may be left empty by a host that never changes its assets.
        std::function<u64(u64 cursor, std::vector<InternedString>& names)> changesSince;
        // The text of a controller asset (a JSON file named like AnimationControllerComponent::controller, relative to
        // the host's animation root): an error that names what failed (a missing file, a name outside the root). May be
        // left empty by a host with no controller assets; an entity naming a controller is then rejected.
        std::function<Expected<std::string>(InternedString controller)> loadController;
        // A counter that grows whenever a file under the controller root may have changed (a directory change
        // notification, no disk read: cheap to call every frame). Each AnimationWorld remembers the value it last saw
        // and, when it differs, reads again the controllers its entities name and rebinds the entities of one whose
        // text changed. May be left empty by a host that never edits controllers while it runs.
        std::function<u64()> controllerVersion;
    };
} // namespace spite
