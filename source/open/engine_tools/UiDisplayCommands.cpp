#include "engine/UiDisplayCommands.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "control/NumberArg.hpp"
#include "control/ControlServer.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiTween.hpp"
#include "uiview/UiStyleSheet.hpp"

namespace spite
{
    namespace
    {
        // nullopt: absent; empty error: usable
        struct ScaleArg
        {
            std::optional<float> value;
            std::string error;
        };

        ScaleArg scaleArg(const ControlArgs& args, const std::string_view key, const float min, const float max)
        {
            if (!args.json(key))
            {
                return {};
            }
            const std::optional<double> number = numberArg(args, key);
            if (!number || !std::isfinite(*number) || *number <= 0.0)
            {
                return {std::nullopt, std::string(key) + " must be a finite number above 0"};
            }
            return {static_cast<float>(std::clamp(*number, static_cast<double>(min), static_cast<double>(max))), {}};
        }
    } // namespace

    void registerUiDisplayCommands(ControlServer& control, UiDisplaySettings& settings,
                                   std::function<float()> displayScale, UiOfFn uiOf)
    {
        control.registerViewCommand(
            "uiDisplay", "engine",
            "sets the UI display settings (UI scale, text scale, reduced motion; all UIs re-lay out once at the next "
            "update) and reports them with the window's display scale and the scale the UI laid out at; a change "
            "is answered once that update ran (settled: true); without arguments it only reports",
            {optionalArg("uiScale", ArgKind::Number, "multiplies the display scale into the layout scale, 0.5 to 4",
                         "unchanged"),
             optionalArg("textScale", ArgKind::Number, "multiplies the text sizes only, 0.5 to 4", "unchanged"),
             optionalArg("reducedMotion", ArgKind::Boolean, "tweens snap to their end value", "unchanged"),
             optionalArg("world", ArgKind::String, "the UI's world to report the layout scale of",
                         "the first UI world, else scene")},
            [&control, &settings, displayScale = std::move(displayScale),
             uiOf = std::move(uiOf)](const ControlArgs& args, IFormatWriter& out)
            {
                const ScaleArg ui =
                    scaleArg(args, "uiScale", UiDisplaySettings::MIN_UI_SCALE, UiDisplaySettings::MAX_UI_SCALE);
                const ScaleArg text =
                    scaleArg(args, "textScale", UiStyleSheet::MIN_TEXT_SCALE, UiStyleSheet::MAX_TEXT_SCALE);
                if (!ui.error.empty() || !text.error.empty())
                {
                    return ui.error.empty() ? text.error : ui.error;
                }
                const UiContext* context = uiOf(args.string("world").value_or(std::string{}));
                if (!context)
                {
                    return std::string("no such UI world");
                }
                if (ui.value)
                {
                    settings.uiScale = *ui.value;
                }
                if (text.value)
                {
                    settings.textScale = *text.value;
                }
                if (const std::optional<bool> reduced = args.boolean("reducedMotion"))
                {
                    settings.reducedMotion = *reduced;
                }
                const auto report = [&settings, &displayScale, &uiOf,
                                     world = args.string("world").value_or(std::string{})](IFormatWriter& result)
                {
                    const UiContext* ui = uiOf(world);
                    if (!ui)
                    {
                        return std::string("no such UI world");
                    }
                    result.writeF32("displayScale", displayScale());
                    result.writeF32("uiScale", settings.uiScale);
                    result.writeF32("textScale", settings.textScale);
                    result.writeBool("reducedMotion", settings.reducedMotion);
                    result.writeF32("layoutScale", ui->hierarchy().layoutScale());
                    result.writeU32("applied", settings.applied);
                    result.writeU32("activeTweens", ui->tweens().activeCount());
                    return std::string{};
                };
                // a change is applied by the next update: answer once it ran
                if (ui.value || text.value || args.boolean("reducedMotion"))
                {
                    return control.replyAfterFrames(1, report, out);
                }
                return report(out);
            });
    }
} // namespace spite
