# RenderConsumer

This standalone module draws a magenta stripe over the composed game view.
The Slang fragment shader discards pixels outside the stripe, leaving the
scene visible around it. The module uses public SPITE headers and import
libraries from the developer SDK.

The pass creates a mapped vertex buffer containing a fullscreen triangle,
declares binding 0 and its two-float attribute in `GraphicsPipelineSpec`, and
binds it before drawing. The module destroys the buffer on unload; the resource
manager retires its GPU allocation after the relevant frame slot finishes.

The module declares a per-view graphics pass at
`RenderPassPlacement::AfterComposite`. Its attachment is `viewTargetName(0)`,
which resolves to the current view's composed output. The attachment load
preserves the scene. In the editor, later view overlays can cover parts of
that output before the UI samples it.

Run the smoke test from this directory in PowerShell. The project path must
be new and its parent directory must exist. Supply a contributor `spite.exe`
from an engine checkout, copied to a directory inside that checkout outside
`bin/<platform>/<configuration>`. This makes its control pipe match the SDK
host while keeping `run` in contributor mode.

```powershell
./Verify.ps1 -Sdk ../SDK -ProjectRoot ../StripeProject -ControlCli <contributor-spite.exe>
```

The script creates an isolated project, builds the module and its Slang
shaders using the SDK, and starts a bounded `SpiteGame.exe` session. It checks
that missing shaders refuse the first load and leave no pass behind. It
checks that a successful load puts `SdkColorStripe` after Composite and that
the game screenshot contains a magenta pixel in the stripe. It verifies that
a malformed fragment is rejected while its previous SPIR-V and visible output
remain in use. Unloading removes the pass and the stripe. The screenshots
are saved in the project's artifacts directory.

To build the same code as a standalone game, configure a separate build directory
with `-DSPITE_COOKED_SDK=<cooked SDK path>`. Use that SDK's
`tools/PublishSdkGame.py` to assemble `RenderConsumer.exe` with the baked
assets and saved scene from the AssetConsumer sample. Pass all four compiled stripe and HDR shaders as `--shader` inputs.

## Pass-level depth bias

The sample also draws a coplanar witness into its own D32 depth attachment.
The first draw writes magenta at device depth 0.5, the second repeats the
same depth with no bias on the left, and the third repeats it with positive
bias on the right. With strict `GREATER` reverse-depth testing the left stays
magenta and the right becomes cyan. The verification script checks both
pixels, so a missing bias or a pipeline cache that aliases the variants fails.

A module sets the state on the spec it supplies to `ShaderManager`:

```cpp
spec.depthTestEnable = true;
spec.depthWriteEnable = true;
spec.depthCompareOp = CompareOp::GREATER;
spec.depthAttachmentFormat = Format::D32_SFLOAT;
spec.depthBiasEnable = true;
spec.depthBiasConstantFactor = 16.0f;
spec.depthBiasSlopeFactor = 0.0f;
```

Declare a matching depth attachment in the graph and choose a factor for the
actual depth format and geometry. Under reverse depth, positive bias increases
device depth and pulls fragments toward the camera. The constant factor is
scaled by the depth representation's minimum resolvable difference; the slope
factor scales the polygon's device-depth slope. These are depth-space terms,
not distances in metres. This state applies to the whole pass pipeline, not
individual materials in ordinary GPU-driven entity draws. Broader per-material
effect-bias semantics remain pending the owner's `effect-bias-semantics` inbox.

## HDR picture-change witness

An `AfterTransparent` pass loads SceneColor and draws a small HDR red patch.
`renderConsumerSetHdrColor` accepts `view: 0` and `color: 0` (red) or `1`
(green). The two preset buffers are immutable; changing the selection marks
view 0 content changed, while repeating the same selection does not. Other
views retain red. The verifier enables TAA and a still camera with auto exposure,
checks red then green in screenshots, and checks that exposure records another
run and settles. It does not require exposure EV to change: the patch may be
trimmed from the histogram. Shader compilation includes both HDR shaders.
