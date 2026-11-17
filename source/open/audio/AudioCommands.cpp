#include "audio/AudioWorld.hpp"
#include "audio/AudioComponents.hpp"
#include "control/ControlServer.hpp"
#include "control/NumberArg.hpp"
#include "ecs/World.hpp"
#include "serialization/FormatWalk.hpp"
#include <cmath>
namespace spite
{
    void registerAudioCommands(ControlServer& control)
    {
        constexpr auto idArg = requiredArg("id", ArgKind::Entity, "the emitter entity");
        for (const auto name : {"playAudio", "pauseAudio", "stopAudio", "reloadAudio"})
        {
            control.registerCommand(name, "audio", "control an emitter in the play world", {WORLD_ARG, idArg},
                                    [&control, name](const ControlArgs& args, IFormatWriter& out)
                                    {
                                        const World* world = control.world(args);
                                        if (!world)
                                            return control.unknownWorld(args);
                                        if (!world->services().contains<AudioWorld>())
                                            return std::string("world has no audio service");
                                        const Entity entity(args.integer("id").value_or(0));
                                        auto audio = world->services().write<AudioWorld>();
                                        const std::string_view command(name);
                                        const bool ok = command == "playAudio" ? audio->play(entity)
                                            : command == "pauseAudio"          ? audio->pause(entity)
                                            : command == "stopAudio"           ? audio->stop(entity)
                                                                               : audio->reload(entity);
                                        if (!ok)
                                            return std::string("entity has no audio emitter");
                                        out.writeBool("available", audio->available());
                                        out.writeF32("masterGain", audio->masterGain());
                                        return std::string{};
                                    });
        }
        control.registerViewCommand("setAudioMasterGain", "audio", "set the world's linear master gain in [0, 1]",
                                    {WORLD_ARG, requiredArg("gain", ArgKind::Number, "linear gain from 0 to 1")},
                                    [&control](const ControlArgs& args, IFormatWriter& out)
                                    {
                                        const World* world = control.world(args);
                                        if (!world)
                                            return control.unknownWorld(args);
                                        if (!world->services().contains<AudioWorld>())
                                            return std::string("world has no audio service");
                                        const auto gain = numberArg(args, "gain");
                                        auto audio = world->services().write<AudioWorld>();
                                        if (!gain || !std::isfinite(*gain) || *gain < 0.0 || *gain > 1.0 ||
                                            !audio->setMasterGain(static_cast<float>(*gain)))
                                            return std::string("gain must be finite and in [0, 1]");
                                        out.writeF32("masterGain", audio->masterGain());
                                        return std::string{};
                                    });
        control.registerViewCommand("setAudioCategoryGain", "audio", "set a category's linear gain in [0, 1]",
                                    {WORLD_ARG,
                                     requiredArg("category", ArgKind::String, "category name; empty is the default"),
                                     requiredArg("gain", ArgKind::Number, "linear gain from 0 to 1")},
                                    [&control](const ControlArgs& args, IFormatWriter& out)
                                    {
                                        const World* world = control.world(args);
                                        if (!world)
                                            return control.unknownWorld(args);
                                        if (!world->services().contains<AudioWorld>())
                                            return std::string("world has no audio service");
                                        const auto gain = numberArg(args, "gain");
                                        const auto name = args.string("category").value_or("");
                                        const InternedString category =
                                            name.empty() ? InternedString{} : InternedString(std::string_view(name));
                                        auto audio = world->services().write<AudioWorld>();
                                        if (!gain || !std::isfinite(*gain) || *gain < 0.0 || *gain > 1.0 ||
                                            !audio->setCategoryGain(category, static_cast<float>(*gain)))
                                            return std::string("gain must be finite and in [0, 1]");
                                        out.writeString("category", name);
                                        out.writeF32("categoryGain", audio->categoryGain(category));
                                        return std::string{};
                                    });
        control.registerQuery("audioStatus", "audio", "audio device and optional emitter status",
                              {WORLD_ARG, optionalArg("id", ArgKind::Entity, "the emitter entity")},
                              [&control](const ControlArgs& args, IFormatWriter& out)
                              {
                                  const World* world = control.world(args);
                                  if (!world)
                                      return control.unknownWorld(args);
                                  if (!world->services().contains<AudioWorld>())
                                      return std::string("world has no audio service");
                                  const auto audio = world->services().read<AudioWorld>();
                                  out.writeBool("available", audio->available());
                                  out.writeF32("masterGain", audio->masterGain());
                                  if (const auto id = args.integer("id"))
                                  {
                                      const Entity entity(*id);
                                      if (!world->isEntityAlive(entity) ||
                                          !world->hasComponent<AudioEmitterComponent>(entity))
                                          return std::string("entity has no audio emitter");
                                      const AudioVoiceStatus status = audio->status(entity);
                                      out.writeBool("loaded", status.loaded);
                                      out.writeBool("playing", status.playing);
                                      out.writeBool("ended", status.ended);
                                      const AudioVoiceDetails details = audio->details(entity);
                                      out.writeF32("pitch", details.pitch);
                                      out.writeF32("minDistance", details.minDistance);
                                      out.writeF32("maxDistance", details.maxDistance);
                                      out.writeF32("rolloff", details.rolloff);
                                      const auto category = details.category;
                                      out.writeString("category",
                                                      category.isValid() ? category.toView() : std::string_view{});
                                      out.writeF32("categoryGain", audio->categoryGain(category));
                                      out.writeF32("effectiveGain", details.effectiveGain);
                                  }
                                  return std::string{};
                              });
    }
} // namespace spite
