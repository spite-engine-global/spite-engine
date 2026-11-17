# Third-party notices

SPITE includes or is built with the third-party software below. Each component stays under its own licence; the
licence name, the copyright line and the licence text (section "Licence texts" at the end, or the link or file named)
are given for each. Where a component offers a choice of licences, the entry names the licence chosen and the
licences offered. SPITE's own licence is in `LICENSE`.

The `Source:` and `Licence:` lines are read by `scripts/checks/CheckLicences.ps1`: every `vcpkg.json` dependency and
every directory under `source/external` needs an entry here, and every `Licence:` must be on the allowlist in
`scripts/LicenceAllowlist.txt`. Licence names are SPDX identifiers; `LicenseRef-*` names are described where they are used.
Source forms: `vcpkg:<port>` (a `vcpkg.json` dependency), `vcpkg-dep:<port>` (a port it pulls in), `bundled:<path>` (code
inside another component), `external:<dir>` (vendored under `source/external`), `font:<file>` (shipped content).

Facts here were read from the licence files on disk and upstream on 2026-10-25; this file is not legal advice.

### miniaudio 0.11.22
- Source: external:miniaudio
- Licence: Unlicense
- Copyright: David Reid and contributors (public domain dedication)
- Text: Unlicense, as offered in source/external/miniaudio/miniaudio.h; the alternative MIT-0 is not elected.
- Upstream: https://github.com/mackron/miniaudio/tree/0.11.22

## 1. Libraries built from the `vcpkg.json` manifest

### Portable SDK build tools
- Source: bundled:sdk/cmake
- Licence: BSD-3-Clause
- Copyright: Copyright Kitware, Inc. and Contributors
- Text: The SDK carries CMake's complete upstream notices under `toolchain/cmake/doc/cmake` (or the versioned doc directory).

### Python (Windows SDK consumer tooling)
- Source: bundled:sdk/python
- Licence: PSF-2.0
- Copyright: Copyright Python Software Foundation and contributors
- Text: `toolchain/python/LICENSE.txt` includes the Python licence and bundled component notices. This interpreter is development tooling, never game runtime.

### SDL3 3.4.16
- Source: vcpkg:sdl3
- Licence: Zlib
- Copyright: Copyright (C) 1997-2026 Sam Lantinga
- Text: Zlib. Windows builds use a shared SDL3.dll.

#### SDL3: HIDAPI
- Source: bundled:sdl3/hidapi
- Licence: BSD-3-Clause
- Copyright: Copyright (c) 2010, Alan Ott, Signal 11 Software
- Text: BSD-3-Clause; clause 3 names "Signal 11 Software".

#### SDL3: stb_image
- Source: bundled:sdl3/stb_image
- Licence: Unlicense
- Copyright: Sean Barrett (public domain, or MIT as offered in the file)
- Text: Unlicense.

#### SDL3: Khronos OpenGL ES headers
- Source: bundled:sdl3/khronos-gles
- Licence: Apache-2.0
- Copyright: Copyright 2013-2020 and 2017-2020 The Khronos Group Inc.
- Text: Apache-2.0 (link).

### Vulkan-Headers and vulkan.hpp 1.4.357.0
- Source: vcpkg:vulkan
- Licence: MIT
- Offered: Apache-2.0 OR MIT. MIT is chosen.
- Copyright: Copyright 2015-2026 The Khronos Group Inc.
- Text: MIT.

#### Vulkan-Loader (import library only)
- Source: vcpkg-dep:vulkan-loader
- Licence: Apache-2.0
- Copyright: Copyright The Khronos Group Inc. and contributors
- Text: Apache-2.0 (link). Only the import library is linked; `vulkan-1.dll` is never copied, the system loader is used.

### Vulkan Memory Allocator 3.4.0
- Source: vcpkg:vulkan-memory-allocator
- Licence: MIT
- Copyright: Copyright (c) 2017-2026 Advanced Micro Devices, Inc.
- Text: MIT.

### VulkanMemoryAllocator-Hpp 3.4.0
- Source: vcpkg:vulkan-memory-allocator-hpp
- Licence: CC0-1.0
- Copyright: none (public-domain dedication)
- Text: https://creativecommons.org/publicdomain/zero/1.0/legalcode

### assimp 6.0.4
- Source: vcpkg:assimp
- Licence: BSD-3-Clause
- Copyright: Copyright (c) 2006-2026, assimp team
- Text: BSD-3-Clause; clause 3 names "the assimp team". Statically linked into `SpiteAsset`. The vcpkg port's own note on
  `test/models-nonbsd` concerns test models that are not part of the library.

#### assimp: OpenDDLParser
- Source: bundled:assimp/openddlparser
- Licence: MIT
- Copyright: Copyright (c) 2014 Kim Kulling
- Text: MIT. Not named in the vcpkg `copyright` file; text from `contrib/openddlparser/LICENSE` of assimp 6.0.4.

#### assimp: Open3DGC
- Source: bundled:assimp/open3dgc
- Licence: MIT
- Copyright: Copyright (c) 2013 Khaled Mammou - Advanced Micro Devices, Inc.
- Text: MIT. From the file header of `contrib/Open3DGC` in assimp 6.0.4.

#### assimp: earcut.hpp
- Source: bundled:assimp/earcut
- Licence: ISC
- Copyright: Copyright (c) 2015, Mapbox
- Text: ISC. Verified against `contrib/earcut-hpp/LICENSE` of assimp 6.0.4.

#### assimp: Draco 1.5.7
- Source: vcpkg-dep:draco
- Licence: Apache-2.0
- Copyright: Copyright The Draco Authors
- Text: Apache-2.0 (link). No NOTICE file exists in Draco 1.5.7.

#### assimp: kubazip 0.3.15 with miniz 3.1.2
- Source: vcpkg-dep:kubazip
- Licence: MIT
- Copyright: Copyright (c) 2013-2014 RAD Game Tools and Valve Software; Copyright (c) 2010-2014 Rich Geldreich and
  Tenacious Software LLC; Copyright (c) 2016 Martin Raiber
- Text: MIT. kubazip's own `LICENSE.txt` names no holder; its upstream (https://github.com/kuba--/zip) states MIT.

#### assimp: minizip 1.3.2
- Source: vcpkg-dep:minizip
- Licence: Zlib
- Copyright: Copyright (c) 1998-2026 Gilles Vollant; ZIP64 by Mathias Svensson (credits: Even Rouault, Daniel Borca)
- Text: Zlib.

#### assimp: zlib 1.3.2
- Source: vcpkg-dep:zlib
- Licence: Zlib
- Copyright: (C) 1995-2026 Jean-loup Gailly and Mark Adler
- Text: Zlib.

#### assimp: poly2tri (jhasse fork)
- Source: vcpkg-dep:jhasse-poly2tri
- Licence: BSD-3-Clause
- Copyright: Copyright (c) 2009-2018, Poly2Tri Contributors
- Text: BSD-3-Clause; clause 3 names "Poly2Tri".

#### assimp: Clipper (polyclipping) 6.4.2
- Source: vcpkg-dep:polyclipping
- Licence: BSL-1.0
- Copyright: Copyright Angus Johnson 2010-2017
- Text: BSL-1.0.

#### assimp: pugixml 1.16
- Source: vcpkg-dep:pugixml
- Licence: MIT
- Copyright: Copyright (c) 2006-2026 Arseny Kapoulkine
- Text: MIT.

#### assimp: RapidJSON
- Source: vcpkg-dep:rapidjson
- Licence: MIT AND BSD-3-Clause
- Copyright: Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip; msinttypes: Copyright (c) 2006-2013
  Alexander Chemeris
- Text: MIT; BSD-3-Clause for `include/rapidjson/msinttypes` (clause 3 names "the copyright holders"). The vcpkg
  `copyright` file also quotes the JSON licence; it covers only `bin/jsonchecker/`, which is not part of this software.

#### assimp: utfcpp 4.2.0
- Source: vcpkg-dep:utfcpp
- Licence: BSL-1.0
- Copyright: Copyright 2006 Nemanja Trifunovic
- Text: BSL-1.0.

### nlohmann/json 3.12.0
- Source: vcpkg:nlohmann-json
- Licence: MIT
- Copyright: Copyright (c) 2013-2025 Niels Lohmann
- Text: MIT.

### SQLite 3.53.4
- Source: vcpkg:sqlite3
- Licence: LicenseRef-PublicDomain
- Copyright: none ("The author disclaims copyright", https://www.sqlite.org/copyright.html)
- Text: public domain; no conditions.

### enkiTS 1.12
- Source: vcpkg:enkits
- Licence: Zlib
- Copyright: Copyright (c) 2013 Doug Binks
- Text: Zlib.

### FreeType 2.14.3
- Source: vcpkg:freetype
- Licence: FTL
- Offered: FTL OR GPL-2.0-or-later. SPITE uses FreeType under the FreeType Project License, not the GNU GPL.
- Copyright: Copyright 1996-2002, 2006 by David Turner, Robert Wilhelm and Werner Lemberg
- Text: FTL (section "Licence texts"). Portions of this software are copyright (c) 2026 The FreeType Project
  (https://freetype.org). All rights reserved. Linked statically into `SpiteUiView`.

#### FreeType: libpng 1.6.58
- Source: vcpkg-dep:libpng
- Licence: libpng-2.0
- Copyright: Copyright (c) 1995-2026 The PNG Reference Library Authors; (c) 2018-2026 Cosmin Truta; (c) 2000-2002,
  2004, 2006-2018 Glenn Randers-Pehrson; (c) 1996-1997 Andreas Dilger; (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.
- Text: libpng-2.0.

#### FreeType: bzip2 1.0.8
- Source: vcpkg-dep:bzip2
- Licence: bzip2-1.0.6
- Copyright: Copyright (C) 1996-2019 Julian R Seward
- Text: bzip2-1.0.6.

#### FreeType: Brotli 1.2.0
- Source: vcpkg-dep:brotli
- Licence: MIT
- Copyright: Copyright (c) 2009, 2010, 2013-2016 by the Brotli Authors
- Text: MIT.

### msdfgen 1.13 (with extensions)
- Source: vcpkg:msdfgen
- Licence: MIT
- Copyright: Copyright (c) 2014-2025 Viktor Chlumsky
- Text: MIT.

#### msdfgen: tinyxml2 11.0.0
- Source: vcpkg-dep:tinyxml2
- Licence: Zlib
- Copyright: Lee Thomason (original code)
- Text: Zlib.

### HarfBuzz 14.4.0
- Source: vcpkg:harfbuzz
- Licence: LicenseRef-Old-MIT AND MIT
- Copyright: Copyright (c) 2010-2022 Google, Inc.; 2015-2020 Ebrahim Byagowi; 2019-2020 Facebook, Inc.; 2012, 2015
  Mozilla Foundation; 2011 Codethink Limited; 2008, 2010 Nokia Corporation; 2009 Keith Stribley; 2011 Martin Hosken and
  SIL International; 2007 Chris Wilson; 2005-2023 Behdad Esfahbod; 2004-2023 Red Hat, Inc.; 1998-2005 David Turner and
  Werner Lemberg; 2016 Igalia S.L.; 2022 Matthias Clasen; 2018, 2021 Khaled Hosny; 2018-2020 Adobe, Inc; 2013-2015
  Alexei Podtelezhnikov; and Microsoft Corporation (a second COPYING file)
- Text: HarfBuzz "Old MIT" (`LicenseRef-Old-MIT`, section "Licence texts") for the main files, MIT for the Microsoft files.

### Slang 2026.18.2 (shader compiler)
- Source: vcpkg:shader-slang
- Licence: Apache-2.0 WITH LLVM-exception
- Copyright: Copyright Slang contributors
- Text: Apache-2.0 (link) with the LLVM exception. Pending (licence audit): the notices for the Slang DLLs
  (`slang.dll`, `slang-compiler.dll`, `slang-glslang.dll`: glslang, SPIRV-Tools, SPIRV-Headers, LZ4, miniz,
  ankerl::unordered_dense, fast_float) and whether a shipped game needs the DLLs at all are decided at the first binary
  release. The compiled shaders are SPITE's own output and carry no Slang licence.

### stb (stb_image, stb_image_write, stb_rect_pack) 2024-07-29
- Source: vcpkg:stb
- Licence: Unlicense
- Offered: MIT OR Unlicense. The public-domain alternative (Unlicense) is chosen.
- Copyright: Sean Barrett (public domain)
- Text: Unlicense.

### googletest 1.18.0 (tests only, not distributed)
- Source: vcpkg:gtest
- Licence: BSD-3-Clause
- Copyright: Copyright 2008, Google Inc.
- Text: BSD-3-Clause; clause 3 names "Google Inc.".

## 2. Vendored source (`source/external`)

### cgltf 1.15
- Source: external:cgltf
- Licence: MIT
- Copyright: Copyright (c) 2018-2021 Johannes Kuhlmann
- Text: MIT; `source/external/cgltf/LICENSE`. Tag v1.15 (commit 360db1a95480fe102ae9c69b27c5d101167ff5ba, 2025-02-09) of
  jkuhlmann/cgltf; `cgltf.h` is the upstream file byte for byte (sha256
  e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec, kept binary by `.gitattributes`). Owner-approved
  2026-10-28 for reading glTF animation samplers only (STEP and CUBICSPLINE, which Assimp 6.0.4 loses): it is private
  to SpiteAsset's `GltfCurveReader.cpp`, no cgltf type is reachable from `include/spite`, and no vertex data is read
  with it.

### Jolt Physics 5.5.1
- Source: external:jolt
- Licence: MIT
- Copyright: Copyright 2021 Jorrit Rouwe
- Text: MIT; `source/external/jolt/LICENSE`. Only `Jolt/` is built. A source drop carries `Jolt/` and `LICENSE` only: the
  rest of the submodule holds other licences (LGPL-3.0 racetrack data, Horizon Zero Dawn assets used with Guerrilla's
  permission, Roboto, doctest, stb_truetype, Perlin).

### meshoptimizer 0.25
- Source: external:meshoptimizer
- Licence: MIT
- Copyright: Copyright (c) 2016-2025 Arseny Kapoulkine
- Text: MIT; `source/external/meshoptimizer/LICENSE.md`. The directory also holds `clusterlod.h`, verbatim from the
  demo of the same repository at commit ad41b4371e04a2134943d2fd81eb4ad724f72fae (2025-10-29, a revision that
  compiles against the vendored 0.25; same author, same MIT notice at the end of the file). It is private to SpiteAsset's
  cluster LOD bake (`source/asset/ClusterLod.cpp`).

### bc7e_scalar (basis_universal encoder, v2.50)
- Source: external:bc7e
- Licence: Apache-2.0
- Copyright: Copyright (C) 2016-2026 Binomial LLC (bc7e, ported to plain C++ by Binomial in basis_universal, commit
  ee771a7f)
- Text: Apache-2.0 (link); `source/external/bc7e/LICENSE`. The directory holds `basisu_bc7e_scalar.cpp` and `.h`
  unmodified, with the repository's `NOTICE`, whose attribution Apache-2.0 4(d) asks every redistribution to carry:
  "Basis Universal" is a trademark of Binomial LLC and is named here only as the source. It encodes the BC7 blocks of
  baked textures and is private to SpiteAsset; no type of it is in `include/spite`. Built with `/fp:precise`.

### rgbcx and bc7decomp (from bc7enc_rdo)
- Source: external:rgbcx
- Licence: MIT
- Offered: MIT OR Unlicense. MIT is chosen.
- Copyright: Copyright (c) 2020-2021 Richard Geldreich, Jr.
- Text: MIT; `source/external/rgbcx/LICENSE`. The directory holds `rgbcx.cpp`, `rgbcx.h`, `rgbcx_table4.h`,
  `bc7decomp.cpp` and `bc7decomp.h` of the bc7enc_rdo repository (commit b9438627), unmodified. rgbcx encodes and decodes
  the BC1, BC4 and BC5 blocks of baked textures; bc7decomp decodes BC7. Private to SpiteAsset. Built with `/fp:precise`.

### mimalloc 2.2.5
- Source: external:mimalloc
- Licence: MIT
- Copyright: Copyright (c) 2018-2025 Microsoft Corporation, Daan Leijen
- Text: MIT; `source/external/mimalloc/LICENSE`.

### ozz-animation 0.17.0
- Source: external:ozz
- Licence: MIT
- Copyright: Copyright (c) 2020 Guillaume Blanc
- Text: MIT; `source/external/ozz/LICENSE.md`. Tag 0.17.0 (commit 744eb9d99f606eda849acb0b1204f7a3dc20bca1) of
  guillaumeblanc/ozz-animation; a source drop carries `include/ozz` and `src` of `base`, `animation/runtime` and
  `animation/offline` (builders and optimizers, without the fbx, gltf and tools importers), plus `LICENSE.md` and
  `AUTHORS.md`. Built as `Ozz.vcxproj`.

### volk (vulkan-sdk-1.4.341.0)
- Source: external:volk
- Licence: MIT
- Copyright: Copyright (c) 2018-2026 Arseny Kapoulkine
- Text: MIT; `source/external/volk/LICENSE.md`.

### RenderDoc in-application API header
- Source: external:renderdoc
- Licence: MIT
- Copyright: Copyright (c) 2015-2026 Baldur Karlsson
- Text: MIT; the header comment of `source/external/renderdoc/renderdoc_app.h`. RenderDoc itself is loaded from the
  user's installation and is never distributed.

### NVIDIA Nsight Aftermath SDK 2.25
- Source: external:NsightAftermath
- Licence: LicenseRef-NVIDIA-Aftermath
- Copyright: Copyright (c) 2017-2023 NVIDIA Corporation
- Text: proprietary, under the NVIDIA SDK licence agreement and the Aftermath supplement; not an open-source licence and
  not reproduced here. Only `GFSDK_Aftermath_Lib.x64.dll` may be distributed, inside an application, and the SDK's
  headers and text files are not distributable. The SDK licence embeds notices for nlohmann/json
  (MIT), FlatBuffers (Apache-2.0) and LLVM (UIUC/NCSA) that are added here with the first binary release that ships the
  DLL. SPITE is not sponsored or endorsed by NVIDIA.

## 3. Fonts and icons (`assets/fonts`)

Each font keeps its licence file in `assets/fonts` beside it; none is sold on its own.

### Chakra Petch Bold
- Source: font:ChakraPetch-Bold
- Licence: OFL-1.1
- Copyright: Copyright 2018 The Chakra Petch Project Authors (https://github.com/m4rc1e/Chakra-Petch.git)
- Text: `assets/fonts/ChakraPetch.LICENSE.txt`.

### Geist Regular, Geist Mono Regular and Medium
- Source: font:Geist
- Licence: OFL-1.1
- Copyright: Copyright 2024 The Geist Project Authors (https://github.com/vercel/geist-font.git)
- Text: `assets/fonts/Geist.LICENSE.txt`, `assets/fonts/GeistMono.LICENSE.txt`.

### JetBrains Mono Regular
- Source: font:JetBrainsMono
- Licence: OFL-1.1
- Copyright: Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono)
- Text: `assets/fonts/LICENSE`; provenance in `assets/fonts/JetBrainsMono.SOURCE.txt`.

### Noto Sans JP (subset)
- Source: font:NotoSansJP
- Licence: OFL-1.1
- Copyright: Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'
- Text: `assets/fonts/NotoSansJPSubset.LICENSE.txt`. The subset's family name is "Noto Sans JP" and does not use 'Source'.

### Noto Sans Arabic and Noto Sans Devanagari (subsets, tests and UiBench only)
- Source: font:NotoSansScripts
- Licence: OFL-1.1
- Copyright: Copyright 2022 The Noto Project Authors (https://github.com/notofonts/arabic, https://github.com/notofonts/devanagari)
- Text: `assets/fonts/NotoSansArabicSubset.LICENSE.txt`, `assets/fonts/NotoSansDevanagariSubset.LICENSE.txt`.

### Phosphor Icons 2.1.2
- Source: font:Phosphor
- Licence: MIT
- Copyright: Copyright (c) 2020-2021 Phosphor Icons
- Text: MIT; `assets/fonts/Phosphor.LICENSE.txt`.

## 4. Acknowledgements

- Unicode CLDR 46: the plural categories and number symbols in `source/localization/LocaleRules.cpp` follow the Unicode CLDR.
- No NVIDIA, Google, assimp team, Poly2Tri, Free Software Foundation or Bartosz Taudul endorsement of SPITE is stated or implied.

## 5. Licence texts

Where an entry gives only a licence name, the text below applies with the entry's copyright line in place of
`<copyright>`.

### MIT

```
MIT License

<copyright>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD-2-Clause

```
<copyright>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### BSD-3-Clause

```
<copyright>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder named in the entry nor the names of
   its contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Zlib

```
<copyright>

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

### ISC

```
ISC License

<copyright>

Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright notice
and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
```

### BSL-1.0

```
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

### Unlicense

```
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### libpng-2.0 (PNG Reference Library License version 2)

```
COPYRIGHT NOTICE, DISCLAIMER, and LICENSE
=========================================

PNG Reference Library License version 2
---------------------------------------

 * Copyright (c) 1995-2026 The PNG Reference Library Authors.
 * Copyright (c) 2018-2026 Cosmin Truta.
 * Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
 * Copyright (c) 1996-1997 Andreas Dilger.
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind,
express or implied, including, without limitation, the warranties
of merchantability, fitness for a particular purpose, title, and
non-infringement.  In no event shall the Copyright owners, or
anyone distributing the software, be liable for any damages or
other liability, whether in contract, tort or otherwise, arising
from, out of, or in connection with the software, or the use or
other dealings in the software, even if advised of the possibility
of such damage.

Permission is hereby granted to use, copy, modify, and distribute
this software, or portions hereof, for any purpose, without fee,
subject to the following restrictions:

 1. The origin of this software must not be misrepresented; you
    must not claim that you wrote the original software.  If you
    use this software in a product, an acknowledgment in the product
    documentation would be appreciated, but is not required.

 2. Altered source versions must be plainly marked as such, and must
    not be misrepresented as being the original software.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.
```

### bzip2-1.0.6

```
This program, "bzip2", the associated library "libbzip2", and all
documentation, are copyright (C) 1996-2019 Julian R Seward.  All
rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. The origin of this software must not be misrepresented; you must
   not claim that you wrote the original software.  If you use this
   software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

3. Altered source versions must be plainly marked as such, and must
   not be misrepresented as being the original software.

4. The name of the author may not be used to endorse or promote
   products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Julian Seward, jseward@acm.org
bzip2/libbzip2 version 1.0.8 of 13 July 2019
```

### LicenseRef-Old-MIT (HarfBuzz "Old MIT")

```
Permission is hereby granted, without written agreement and without
license or royalty fees, to use, copy, modify, and distribute this
software and its documentation for any purpose, provided that the
above copyright notice and the following two paragraphs appear in
all copies of this software.

IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
```

### FTL (FreeType Project License)

```
                    The FreeType Project LICENSE
                    ----------------------------

                            2006-Jan-27

                    Copyright 1996-2002, 2006 by
          David Turner, Robert Wilhelm, and Werner Lemberg



Introduction
============

  The FreeType  Project is distributed in  several archive packages;
  some of them may contain, in addition to the FreeType font engine,
  various tools and  contributions which rely on, or  relate to, the
  FreeType Project.

  This  license applies  to all  files found  in such  packages, and
  which do not  fall under their own explicit  license.  The license
  affects  thus  the  FreeType   font  engine,  the  test  programs,
  documentation and makefiles, at the very least.

  This  license   was  inspired  by  the  BSD,   Artistic,  and  IJG
  (Independent JPEG  Group) licenses, which  all encourage inclusion
  and  use of  free  software in  commercial  and freeware  products
  alike.  As a consequence, its main points are that:

    o We don't promise that this software works. However, we will be
      interested in any kind of bug reports. (`as is' distribution)

    o You can  use this software for whatever you  want, in parts or
      full form, without having to pay us. (`royalty-free' usage)

    o You may not pretend that  you wrote this software.  If you use
      it, or  only parts of it,  in a program,  you must acknowledge
      somewhere  in  your  documentation  that  you  have  used  the
      FreeType code. (`credits')

  We  specifically  permit  and  encourage  the  inclusion  of  this
  software, with  or without modifications,  in commercial products.
  We  disclaim  all warranties  covering  The  FreeType Project  and
  assume no liability related to The FreeType Project.


  Finally,  many  people  asked  us  for  a  preferred  form  for  a
  credit/disclaimer to use in compliance with this license.  We thus
  encourage you to use the following text:

   """
    Portions of this software are copyright © <year> The FreeType
    Project (https://freetype.org).  All rights reserved.
   """

  Please replace <year> with the value from the FreeType version you
  actually use.


Legal Terms
===========

0. Definitions
--------------

  Throughout this license,  the terms `package', `FreeType Project',
  and  `FreeType  archive' refer  to  the  set  of files  originally
  distributed  by the  authors  (David Turner,  Robert Wilhelm,  and
  Werner Lemberg) as the `FreeType Project', be they named as alpha,
  beta or final release.

  `You' refers to  the licensee, or person using  the project, where
  `using' is a generic term including compiling the project's source
  code as  well as linking it  to form a  `program' or `executable'.
  This  program is  referred to  as  `a program  using the  FreeType
  engine'.

  This  license applies  to all  files distributed  in  the original
  FreeType  Project,   including  all  source   code,  binaries  and
  documentation,  unless  otherwise  stated   in  the  file  in  its
  original, unmodified form as  distributed in the original archive.
  If you are  unsure whether or not a particular  file is covered by
  this license, you must contact us to verify this.

  The FreeType  Project is copyright (C) 1996-2000  by David Turner,
  Robert Wilhelm, and Werner Lemberg.  All rights reserved except as
  specified below.

1. No Warranty
--------------

  THE FREETYPE PROJECT  IS PROVIDED `AS IS' WITHOUT  WARRANTY OF ANY
  KIND, EITHER  EXPRESS OR IMPLIED,  INCLUDING, BUT NOT  LIMITED TO,
  WARRANTIES  OF  MERCHANTABILITY   AND  FITNESS  FOR  A  PARTICULAR
  PURPOSE.  IN NO EVENT WILL ANY OF THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE  FOR ANY DAMAGES CAUSED  BY THE USE OR  THE INABILITY TO
  USE, OF THE FREETYPE PROJECT.

2. Redistribution
-----------------

  This  license  grants  a  worldwide, royalty-free,  perpetual  and
  irrevocable right  and license to use,  execute, perform, compile,
  display,  copy,   create  derivative  works   of,  distribute  and
  sublicense the  FreeType Project (in  both source and  object code
  forms)  and  derivative works  thereof  for  any  purpose; and  to
  authorize others  to exercise  some or all  of the  rights granted
  herein, subject to the following conditions:

    o Redistribution of  source code  must retain this  license file
      (`FTL.TXT') unaltered; any  additions, deletions or changes to
      the original  files must be clearly  indicated in accompanying
      documentation.   The  copyright   notices  of  the  unaltered,
      original  files must  be  preserved in  all  copies of  source
      files.

    o Redistribution in binary form must provide a  disclaimer  that
      states  that  the software is based in part of the work of the
      FreeType Team,  in  the  distribution  documentation.  We also
      encourage you to put an URL to the FreeType web page  in  your
      documentation, though this isn't mandatory.

  These conditions  apply to any  software derived from or  based on
  the FreeType Project,  not just the unmodified files.   If you use
  our work, you  must acknowledge us.  However, no  fee need be paid
  to us.

3. Advertising
--------------

  Neither the  FreeType authors and  contributors nor you  shall use
  the name of the  other for commercial, advertising, or promotional
  purposes without specific prior written permission.

  We suggest,  but do not require, that  you use one or  more of the
  following phrases to refer  to this software in your documentation
  or advertising  materials: `FreeType Project',  `FreeType Engine',
  `FreeType library', or `FreeType Distribution'.

  As  you have  not signed  this license,  you are  not  required to
  accept  it.   However,  as  the FreeType  Project  is  copyrighted
  material, only  this license, or  another one contracted  with the
  authors, grants you  the right to use, distribute,  and modify it.
  Therefore,  by  using,  distributing,  or modifying  the  FreeType
  Project, you indicate that you understand and accept all the terms
  of this license.

4. Contacts
-----------

  There are two mailing lists related to FreeType:

    o freetype@nongnu.org

      Discusses general use and applications of FreeType, as well as
      future and  wanted additions to the  library and distribution.
      If  you are looking  for support,  start in  this list  if you
      haven't found anything to help you in the documentation.

    o freetype-devel@nongnu.org

      Discusses bugs,  as well  as engine internals,  design issues,
      specific licenses, porting, etc.

  Our home page can be found at

    https://freetype.org
```

### Apache-2.0, OFL-1.1, CC0-1.0

- Apache-2.0: https://www.apache.org/licenses/LICENSE-2.0.txt (the full text ships with any binary release that includes
  Draco or the Slang DLLs).
- OFL-1.1: the full text is in each font's licence file in `assets/fonts`, named in its entry.
- CC0-1.0: https://creativecommons.org/publicdomain/zero/1.0/legalcode
