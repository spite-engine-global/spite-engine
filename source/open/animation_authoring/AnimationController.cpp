#include "animation/AnimationControllerAsset.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "base/Error.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Parameters = std::vector<ControllerParameter>;

        // Reads one object with a fixed key set: the first problem is kept and names where it is.
        class Reader
        {
        public:
            Reader(const Json& object, std::string where, std::string& error) :
                m_object(object), m_where(std::move(where)), m_error(error)
            {
                if (!m_object.is_object())
                {
                    fail("is not an object");
                    return;
                }
                m_checked = true;
            }

            [[nodiscard]] bool ok() const { return m_checked && m_error.empty(); }

            // an unknown key is an error (a typo must not silently do nothing)
            void only(std::initializer_list<const char*> keys)
            {
                if (!ok())
                {
                    return;
                }
                for (const auto& [key, value] : m_object.items())
                {
                    bool known = false;
                    for (const char* allowed : keys)
                    {
                        known = known || key == allowed;
                    }
                    if (!known)
                    {
                        fail(std::format("has the unknown key '{}'", key));
                        return;
                    }
                }
            }

            [[nodiscard]] const Json* find(const char* key) const
            {
                const auto it = m_object.find(key);
                return it == m_object.end() ? nullptr : &*it;
            }

            bool text(const char* key, std::string& out, const bool required)
            {
                const Json* value = ok() ? find(key) : nullptr;
                if (!value)
                {
                    if (ok() && required)
                    {
                        fail(std::format("is missing '{}'", key));
                    }
                    return false;
                }
                if (!value->is_string() || value->get_ref<const std::string&>().empty())
                {
                    fail(std::format("'{}' must be a non-empty string", key));
                    return false;
                }
                out = value->get<std::string>();
                return true;
            }

            bool number(const char* key, f32& out, const bool nonNegative = false)
            {
                const Json* value = ok() ? find(key) : nullptr;
                if (!value)
                {
                    return false;
                }
                // finite as the float that is stored: 1e300 is a finite double and an infinite float
                if (!value->is_number() || !std::isfinite(value->get<f32>()) ||
                    (nonNegative && value->get<double>() < 0.0))
                {
                    fail(
                        std::format("'{}' must be a {}number", key, nonNegative ? "finite, non-negative " : "finite "));
                    return false;
                }
                out = value->get<f32>();
                return true;
            }

            bool boolean(const char* key, bool& out)
            {
                const Json* value = ok() ? find(key) : nullptr;
                if (!value)
                {
                    return false;
                }
                if (!value->is_boolean())
                {
                    fail(std::format("'{}' must be true or false", key));
                    return false;
                }
                out = value->get<bool>();
                return true;
            }

            // an array of objects, or null when absent
            const Json* array(const char* key, const bool required)
            {
                const Json* value = ok() ? find(key) : nullptr;
                if (!value)
                {
                    if (ok() && required)
                    {
                        fail(std::format("is missing '{}'", key));
                    }
                    return nullptr;
                }
                if (!value->is_array())
                {
                    fail(std::format("'{}' must be an array", key));
                    return nullptr;
                }
                return value;
            }

            void fail(const std::string& what)
            {
                if (m_error.empty())
                {
                    m_error = std::format("{} {}", m_where, what);
                }
            }

        private:
            const Json& m_object;
            std::string m_where;
            std::string& m_error;
            bool m_checked = false;
        };

        bool compareOf(const std::string& op, ControllerCompare& out)
        {
            static const std::unordered_map<std::string, ControllerCompare> ops = {
                {">", ControllerCompare::Greater}, {">=", ControllerCompare::GreaterEqual},
                {"<", ControllerCompare::Less},    {"<=", ControllerCompare::LessEqual},
                {"==", ControllerCompare::Equal},  {"!=", ControllerCompare::NotEqual}};
            const auto it = ops.find(op);
            if (it == ops.end())
            {
                return false;
            }
            out = it->second;
            return true;
        }

        const char* kindName(const AnimationParameterKind kind)
        {
            return kind == AnimationParameterKind::Float ? "float"
                : kind == AnimationParameterKind::Bool   ? "bool"
                                                         : "trigger";
        }

        void readParameters(const Json& list, AnimationControllerDef& def, std::string& error)
        {
            for (sizet i = 0; i < list.size() && error.empty(); ++i)
            {
                Reader reader(list[i], std::format("parameter #{}", i), error);
                reader.only({"name", "type", "default"});
                std::string name;
                std::string type;
                reader.text("name", name, true);
                reader.text("type", type, true);
                if (!reader.ok())
                {
                    continue;
                }
                ControllerParameter parameter;
                parameter.name = InternedString(name);
                if (type == "float")
                {
                    parameter.kind = AnimationParameterKind::Float;
                }
                else if (type == "bool")
                {
                    parameter.kind = AnimationParameterKind::Bool;
                }
                else if (type == "trigger")
                {
                    parameter.kind = AnimationParameterKind::Trigger;
                }
                else
                {
                    reader.fail(std::format("'{}' has the unknown type '{}' (float, bool or trigger)", name, type));
                    continue;
                }
                for (const ControllerParameter& other : def.parameters)
                {
                    if (other.name == parameter.name)
                    {
                        reader.fail(std::format("'{}' is declared twice", name));
                    }
                }
                if (reader.find("default") && reader.ok())
                {
                    bool flag = false;
                    if (parameter.kind == AnimationParameterKind::Trigger)
                    {
                        reader.fail(std::format("'{}' is a trigger: it has no default", name));
                    }
                    else if (parameter.kind == AnimationParameterKind::Bool && reader.boolean("default", flag))
                    {
                        parameter.initial = flag ? 1.0f : 0.0f;
                    }
                    else if (parameter.kind == AnimationParameterKind::Float)
                    {
                        reader.number("default", parameter.initial);
                    }
                }
                if (reader.ok())
                {
                    def.parameters.push_back(parameter);
                }
            }
        }

        template <typename Item, typename Name>
        i32 indexOf(const std::vector<Item>& items, const std::string& name, Name Item::* member)
        {
            const InternedString key(name);
            for (sizet i = 0; i < items.size(); ++i)
            {
                if (items[i].*member == key)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        void readTreeParameter(Reader& reader, const Parameters& parameters, const std::string& name, u32& out)
        {
            const i32 index = indexOf(parameters, name, &ControllerParameter::name);
            if (index < 0)
            {
                reader.fail(std::format("names the unknown parameter '{}'", name));
            }
            else if (parameters[static_cast<sizet>(index)].kind != AnimationParameterKind::Float)
            {
                reader.fail(std::format("parameter '{}' must be a float", name));
            }
            else
            {
                out = static_cast<u32>(index);
            }
        }

        void readTreeChild(const Json& item, const std::string& where, const bool oneD, ControllerBlendTree& tree,
                           std::string& error)
        {
            Reader reader(item, where, error);
            reader.only({"clip", "threshold", "position"});
            std::string clip;
            BlendTreeChild child;
            reader.text("clip", clip, true);
            const Json* position = reader.ok() ? reader.find("position") : nullptr;
            if (oneD && position)
            {
                reader.fail("is in a 1D tree: it takes 'threshold', not 'position'");
            }
            else if (oneD && !reader.find("threshold"))
            {
                reader.fail("is missing 'threshold'");
            }
            else if (oneD)
            {
                reader.number("threshold", child.x);
                if (reader.ok() && !tree.children.empty() && !(child.x > tree.children.back().x))
                {
                    reader.fail("has a threshold that does not rise above the one before it");
                }
            }
            else if (reader.ok() && reader.find("threshold"))
            {
                reader.fail("is in a 2D tree: it takes 'position', not 'threshold'");
            }
            else if (reader.ok() && !position)
            {
                reader.fail("is missing 'position' ([x, y])");
            }
            else if (reader.ok())
            {
                const auto finite = [](const Json& value)
                { return value.is_number() && std::isfinite(value.get<f32>()); };
                if (!position->is_array() || position->size() != 2 || !finite((*position)[0]) ||
                    !finite((*position)[1]))
                {
                    reader.fail("'position' must be two finite numbers");
                }
                else
                {
                    child.x = (*position)[0].get<f32>();
                    child.y = (*position)[1].get<f32>();
                    for (const BlendTreeChild& other : tree.children)
                    {
                        if (other.x == child.x && other.y == child.y)
                        {
                            reader.fail("stands where an earlier child does");
                        }
                    }
                }
            }
            if (reader.ok())
            {
                child.clip = InternedString(clip);
                tree.children.push_back(child);
            }
        }

        void readBlendTree(const Json& item, const Parameters& parameters, const std::string& where,
                           ControllerBlendTree& tree, std::string& error)
        {
            Reader reader(item, where + " blendTree", error);
            reader.only({"type", "parameter", "parameters", "children"});
            std::string type;
            reader.text("type", type, true);
            if (reader.ok() && type != "1D" && type != "2D")
            {
                reader.fail(std::format("has the unknown type '{}' (1D or 2D)", type));
            }
            const bool oneD = type == "1D";
            tree.kind = oneD ? BlendTreeKind::OneD : BlendTreeKind::TwoD;
            std::string name;
            if (!reader.ok())
            {
                return;
            }
            if (oneD && reader.find("parameters"))
            {
                reader.fail("is 1D: it takes 'parameter', not 'parameters'");
            }
            else if (!oneD && reader.find("parameter"))
            {
                reader.fail("is 2D: it takes 'parameters' (two names), not 'parameter'");
            }
            else if (oneD && reader.text("parameter", name, true))
            {
                readTreeParameter(reader, parameters, name, tree.parameters[0]);
            }
            else if (!oneD)
            {
                const Json* names = reader.array("parameters", true);
                if (names && (names->size() != 2 || !(*names)[0].is_string() || !(*names)[1].is_string()))
                {
                    reader.fail("'parameters' must hold two parameter names");
                }
                for (u32 p = 0; names && reader.ok() && p < 2; ++p)
                {
                    readTreeParameter(reader, parameters, (*names)[p].get<std::string>(), tree.parameters[p]);
                }
            }
            const Json* children = reader.array("children", true);
            if (children && (children->empty() || children->size() > MAX_BLEND_TREE_CHILDREN))
            {
                reader.fail(std::format("'children' must hold 1 to {} children", MAX_BLEND_TREE_CHILDREN));
            }
            for (sizet c = 0; children && c < children->size() && error.empty(); ++c)
            {
                readTreeChild((*children)[c], std::format("{} blendTree child #{}", where, c), oneD, tree, error);
            }
        }

        void readStates(const Json& list, const Parameters& parameters, ControllerStateMachine& machine,
                        const std::string& scope, std::string& error)
        {
            for (sizet i = 0; i < list.size() && error.empty(); ++i)
            {
                const std::string where = std::format("{}state #{}", scope, i);
                Reader reader(list[i], where, error);
                reader.only({"name", "clip", "blendTree", "speed", "loop"});
                std::string name;
                std::string clip;
                ControllerState state;
                reader.text("name", name, true);
                const bool hasClip = reader.text("clip", clip, false);
                const Json* tree = reader.ok() ? reader.find("blendTree") : nullptr;
                if (reader.ok() && hasClip == (tree != nullptr))
                {
                    reader.fail("needs exactly one of 'clip' and 'blendTree'");
                }
                if (tree)
                {
                    readBlendTree(*tree, parameters, where, state.tree, error);
                }
                reader.number("speed", state.speed);
                reader.boolean("loop", state.loop);
                if (!reader.ok())
                {
                    continue;
                }
                state.name = InternedString(name);
                state.clip = tree ? state.tree.children.front().clip : InternedString(clip);
                for (const ControllerState& other : machine.states)
                {
                    if (other.name == state.name)
                    {
                        reader.fail(std::format("'{}' is declared twice", name));
                    }
                }
                if (reader.ok())
                {
                    machine.states.push_back(state);
                }
            }
        }

        void readCondition(const Json& item, const Parameters& parameters, const std::string& where,
                           ControllerTransition& transition, std::string& error)
        {
            Reader reader(item, where, error);
            reader.only({"parameter", "op", "value"});
            std::string parameterName;
            reader.text("parameter", parameterName, true);
            if (!reader.ok())
            {
                return;
            }
            const i32 index = indexOf(parameters, parameterName, &ControllerParameter::name);
            if (index < 0)
            {
                reader.fail(std::format("names the unknown parameter '{}'", parameterName));
                return;
            }
            const ControllerParameter& parameter = parameters[static_cast<sizet>(index)];
            ControllerCondition condition{.parameter = static_cast<u32>(index)};
            std::string op;
            const bool hasOp = reader.text("op", op, false);
            if (parameter.kind == AnimationParameterKind::Trigger)
            {
                if (hasOp || reader.find("value"))
                {
                    reader.fail(
                        std::format("'{}' is a trigger: a condition on it takes no op and no value", parameterName));
                }
                condition.compare = ControllerCompare::Triggered;
            }
            else if (parameter.kind == AnimationParameterKind::Bool)
            {
                bool flag = true;
                if (!reader.find("value"))
                {
                    reader.fail(std::format("on bool '{}' is missing 'value' (true or false)", parameterName));
                }
                reader.boolean("value", flag);
                condition.compare = ControllerCompare::Equal;
                if (hasOp && !(op == "==" || op == "!="))
                {
                    reader.fail(std::format("on bool '{}': op must be == or !=, not '{}'", parameterName, op));
                }
                else if (op == "!=")
                {
                    condition.compare = ControllerCompare::NotEqual;
                }
                condition.value = flag ? 1.0f : 0.0f;
            }
            else
            {
                if (!hasOp || !compareOf(op, condition.compare))
                {
                    reader.fail(
                        std::format("on float '{}': op must be one of > >= < <= == != (got '{}')", parameterName, op));
                }
                if (!reader.find("value"))
                {
                    reader.fail(std::format("on float '{}' is missing 'value'", parameterName));
                }
                reader.number("value", condition.value);
            }
            if (reader.ok())
            {
                transition.conditions.push_back(condition);
            }
        }

        void readTransitions(const Json& list, const Parameters& parameters, ControllerStateMachine& machine,
                             const std::string& scope, std::string& error)
        {
            for (sizet i = 0; i < list.size() && error.empty(); ++i)
            {
                Reader probe(list[i], std::format("{}transition #{}", scope, i), error);
                probe.only({"from", "to", "duration", "exitTime", "conditions"});
                std::string from;
                std::string to;
                probe.text("from", from, true);
                probe.text("to", to, true);
                if (!probe.ok())
                {
                    continue;
                }
                const std::string where = std::format("{}transition #{} ({} -> {})", scope, i, from, to);
                Reader reader(list[i], where, error);
                ControllerTransition transition;
                if (from != "*")
                {
                    transition.from = indexOf(machine.states, from, &ControllerState::name);
                    if (transition.from < 0)
                    {
                        reader.fail(std::format("names the unknown state '{}'", from));
                    }
                }
                const i32 target = indexOf(machine.states, to, &ControllerState::name);
                if (target < 0)
                {
                    reader.fail(std::format("names the unknown state '{}'", to));
                }
                else if (target == transition.from)
                {
                    reader.fail("leads from a state to itself");
                }
                transition.to = static_cast<u32>(std::max(target, 0));
                reader.number("duration", transition.duration, true);
                reader.number("exitTime", transition.exitTime, true);
                if (const Json* conditions = reader.array("conditions", false))
                {
                    for (sizet c = 0; c < conditions->size() && error.empty(); ++c)
                    {
                        readCondition((*conditions)[c], parameters, std::format("{}: condition #{}", where, c),
                                      transition, error);
                    }
                }
                if (error.empty() && transition.conditions.empty() && transition.exitTime < 0.0f)
                {
                    reader.fail("has no condition and no exitTime: it would fire at once, always");
                }
                if (error.empty())
                {
                    machine.transitions.push_back(std::move(transition));
                }
            }
        }

        // the keys of a state machine: states, initial and transitions, on the base and on each layer
        void readMachine(Reader& reader, const Parameters& parameters, ControllerStateMachine& machine,
                         const std::string& scope, std::string& error)
        {
            if (const Json* states = reader.array("states", true))
            {
                if (states->empty())
                {
                    reader.fail("'states' is empty: a controller needs at least one state");
                }
                readStates(*states, parameters, machine, scope, error);
            }
            std::string initial;
            if (reader.text("initial", initial, false))
            {
                const i32 index = indexOf(machine.states, initial, &ControllerState::name);
                if (index < 0)
                {
                    reader.fail(std::format("'initial' names the unknown state '{}'", initial));
                }
                machine.initial = static_cast<u32>(std::max(index, 0));
            }
            if (const Json* transitions = reader.array("transitions", false))
            {
                readTransitions(*transitions, parameters, machine, scope, error);
            }
        }

        void readMask(const Json& item, const std::string& where, ControllerJointMask& mask, std::string& error)
        {
            Reader reader(item, where + " mask", error);
            reader.only({"root", "weight", "joints"});
            std::string root;
            if (reader.text("root", root, false))
            {
                mask.root = InternedString(root);
            }
            reader.number("weight", mask.weight, true);
            if (reader.ok() && mask.weight > 1.0f)
            {
                reader.fail("'weight' must be from 0 to 1");
            }
            const Json* joints = reader.array("joints", false);
            for (sizet j = 0; joints && j < joints->size() && error.empty(); ++j)
            {
                Reader entry((*joints)[j], std::format("{} mask joint #{}", where, j), error);
                entry.only({"joint", "weight"});
                std::string joint;
                ControllerMaskJoint masked;
                entry.text("joint", joint, true);
                if (entry.ok() && !entry.find("weight"))
                {
                    entry.fail("is missing 'weight'");
                }
                entry.number("weight", masked.weight, true);
                if (entry.ok() && masked.weight > 1.0f)
                {
                    entry.fail("'weight' must be from 0 to 1");
                }
                masked.joint = InternedString(joint);
                if (entry.ok() &&
                    std::ranges::any_of(mask.joints,
                                        [&](const ControllerMaskJoint& other) { return other.joint == masked.joint; }))
                {
                    entry.fail(std::format("names the joint '{}' twice", joint));
                }
                if (entry.ok())
                {
                    mask.joints.push_back(masked);
                }
            }
        }

        void readLayers(const Json& list, AnimationControllerDef& def, std::string& error)
        {
            if (list.size() > MAX_CONTROLLER_LAYERS)
            {
                error =
                    std::format("document 'layers' holds {} layers: at most {}", list.size(), MAX_CONTROLLER_LAYERS);
                return;
            }
            for (sizet i = 0; i < list.size() && error.empty(); ++i)
            {
                const std::string where = std::format("layer #{}", i);
                Reader reader(list[i], where, error);
                reader.only(
                    {"name", "additive", "weight", "weightParameter", "mask", "states", "initial", "transitions"});
                ControllerLayer layer;
                std::string name;
                std::string weightParameter;
                reader.text("name", name, true);
                reader.boolean("additive", layer.additive);
                const bool hasWeight = reader.number("weight", layer.weight, true);
                if (reader.text("weightParameter", weightParameter, false))
                {
                    if (hasWeight)
                    {
                        reader.fail("takes 'weight' or 'weightParameter', not both");
                    }
                    u32 index = 0;
                    readTreeParameter(reader, def.parameters, weightParameter, index);
                    layer.weightParameter = static_cast<i32>(index);
                }
                if (const Json* mask = reader.ok() ? reader.find("mask") : nullptr)
                {
                    readMask(*mask, where, layer.mask, error);
                }
                readMachine(reader, def.parameters, layer, where + " ", error);
                layer.name = InternedString(name);
                if (reader.ok() &&
                    std::ranges::any_of(def.layers,
                                        [&](const ControllerLayer& other) { return other.name == layer.name; }))
                {
                    reader.fail(std::format("'{}' is declared twice", name));
                }
                if (reader.ok())
                {
                    def.layers.push_back(std::move(layer));
                }
            }
        }

        // the first clip a state (or tree child) names that the host does not know, as an error text
        std::string unknownClip(const ControllerStateMachine& machine, const std::string& scope,
                                const std::function<bool(InternedString)>& clipExists)
        {
            for (const ControllerState& state : machine.states)
            {
                if (!state.hasTree() && !clipExists(state.clip))
                {
                    return std::format("{}state '{}' names the unknown clip '{}'", scope, state.name.toView(),
                                       state.clip.toView());
                }
                for (sizet c = 0; c < state.tree.children.size(); ++c)
                {
                    if (!clipExists(state.tree.children[c].clip))
                    {
                        return std::format("{}state '{}' blendTree child #{} names the unknown clip '{}'", scope,
                                           state.name.toView(), c, state.tree.children[c].clip.toView());
                    }
                }
            }
            return {};
        }

        std::string build(const Json& root, const InternedString name, AnimationControllerDef& def,
                          const std::function<bool(InternedString)>& clipExists)
        {
            std::string error;
            Reader reader(root, "document", error);
            reader.only({"version", "parameters", "states", "initial", "transitions", "layers"});
            const Json* version = reader.ok() ? reader.find("version") : nullptr;
            if (reader.ok() && !version)
            {
                reader.fail("is missing 'version'");
            }
            else if (reader.ok() && (!version->is_number_integer() || version->get<std::int64_t>() != 1))
            {
                reader.fail(std::format("has version {}: this build reads version 1", version->dump()));
            }
            def.name = name;
            if (const Json* parameters = reader.array("parameters", false))
            {
                readParameters(*parameters, def, error);
            }
            readMachine(reader, def.parameters, def, {}, error);
            if (const Json* layers = reader.array("layers", false))
            {
                readLayers(*layers, def, error);
            }
            if (error.empty() && clipExists)
            {
                error = unknownClip(def, {}, clipExists);
                for (sizet i = 0; i < def.layers.size() && error.empty(); ++i)
                {
                    error = unknownClip(def.layers[i], std::format("layer '{}': ", def.layers[i].name.toView()),
                                        clipExists);
                }
            }
            return error;
        }
    } // namespace

    Expected<AnimationControllerDef> parseAnimationController(const std::string_view text, const InternedString name,
                                                              const std::function<bool(InternedString)>& clipExists)
    {
        const auto fail = [&](const ErrorCode code, const std::string& why)
        { return std::unexpected(makeError(code, std::format("controller '{}': {}", name.toView(), why))); };
        Json root;
        try
        {
            root = Json::parse(text);
        }
        catch (const Json::exception& e)
        {
            return fail(ErrorCode::JsonParseFailed, e.what());
        }
        AnimationControllerDef def;
        if (std::string error = build(root, name, def, clipExists); !error.empty())
        {
            return fail(ErrorCode::AssetLoadFailed, error);
        }
        return def;
    }
} // namespace spite
