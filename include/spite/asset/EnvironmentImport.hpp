#pragma once
#include <span>
#include <string>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/EnvironmentAsset.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "asset/TextureMips.hpp"

namespace spite
{
    // The environment asset: an equirectangular HDR image with a mip chain, baked to a .spiteenv. The GPU turns it
    // into a cube; every source layout is brought to this one on import.
    //
    // Equirect convention (the GPU bake uses the same): world space is right-handed, +Y up. A texel centre (u, v),
    // both 0..1 from the image's top-left, looks along
    //   phi = (u - 0.5) * 2 pi, theta = v * pi, direction = (sin(theta) sin(phi), cos(theta), -sin(theta) cos(phi))
    // so the image centre looks down -Z, u grows towards +X and the top row looks up. The inverse is
    //   u = atan2(d.x, -d.z) / (2 pi) + 0.5, v = acos(d.y) / pi.
    // Cube faces follow the cube map convention of Vulkan and OpenGL (faces +X, -X, +Y, -Y, +Z, -Z, image row 0 on
    // top): a direction r picks the axis of its largest component, with s = (sc / |ma| + 1) / 2 along the face's
    // columns and t = (tc / |ma| + 1) / 2 down its rows, where
    //   +X: ma = rx, sc = -rz, tc = -ry      -X: ma = -rx, sc = +rz, tc = -ry
    //   +Y: ma = ry, sc = +rx, tc = +rz      -Y: ma = -ry, sc = +rx, tc = -rz
    //   +Z: ma = rz, sc = +rx, tc = -ry      -Z: ma = -rz, sc = -rx, tc = -ry
    struct HdrImage
    {
        u32 width = 0;
        u32 height = 0;
        std::vector<float> rgb; // linear, width * height * 3, row 0 on top
    };

    enum class EnvironmentFormat : u8
    {
        Rgba16Float, // R16G16B16A16_SFLOAT, alpha 1: the default
        Rgb9E5, // E5B9G9R9_UFLOAT: half the bytes, a shared exponent
        R11G11B10, // B10G11R11_UFLOAT
    };

    // The cube faces' file stems in face order, for a source folder: px, nx, py, ny, pz, nz.
    inline constexpr const char* ENVIRONMENT_FACE_NAMES[6] = {"px", "nx", "py", "ny", "pz", "nz"};

    // Reads an environment source: an equirectangular image (width twice the height), a horizontal (4 x 3 cells) or
    // vertical (3 x 4 cells) cube cross, or a folder holding the six square faces px, nx, py, ny, pz, nz (any
    // image format stb reads). A cross and six faces come back as the equirect of four times the face's width: the
    // same layout whatever the source. `path` is UTF-8. A source over MAX_ENVIRONMENT_EXTENT (a face over a quarter
    // of it) or of another aspect is refused before its pixels are decoded. A Radiance .hdr keeps its range; any
    // other format goes through stb's LDR-to-HDR conversion (gamma 2.2, not sRGB, with the C library's pow), so only
    // .hdr sources and encodeEnvironment input bake to the same bytes on every machine.
    //
    // Cross cells, (column, row) from the top-left, every arm upright (its image row 0 on top) unless noted:
    //   horizontal, 4 x 3:  +Y (1,0)   -X (0,1) +Z (1,1) +X (2,1) -Z (3,1)   -Y (1,2)
    //   vertical,   3 x 4:  +Y (1,0)   -X (0,1) +Z (1,1) +X (2,1)            -Y (1,2)   -Z (1,3) turned 180 degrees
    // (the vertical cross hangs -Z below -Y, so its top edge meets the bottom edge of -Y; read back turned half a
    // circle it is the -Z face the cube map convention above describes).
    SPITE_ASSET_API Expected<HdrImage> importEnvironmentSource(const std::string& path);

    // Six square faces as the equirect image, sampled bilinearly per texel centre.
    SPITE_ASSET_API Expected<HdrImage> cubeFacesToEquirect(std::span<const HdrImage, 6> faces);

    // The .spiteenv bytes of an equirect image: the header, then every mip level (the box-filtered chain, down to
    // 1 x 1) in the format, largest first. Deterministic: the same image and format give the same bytes in every
    // build configuration.
    SPITE_ASSET_API Expected<std::vector<std::byte>> encodeEnvironment(const HdrImage& equirect,
                                                                       EnvironmentFormat format);

    inline u32 environmentMipLevels(const HdrImage& image) { return textureMipCount(image.width, image.height); }
} // namespace spite
