/*
 * Copyright (C) 2026 Smirnov Vladimir / mapron1@gmail.com
 * SPDX-License-Identifier: BSL-1.0
 * See LICENSE file for details.
 */
#pragma once

#include "MernelPlatform/PropertyTree.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace D2ModGen {

struct ConfigHasher {
    uint32_t hashv = 2166136261u;

    void add(bool value)
    {
        uint8_t byte = value ? 1 : 0;
        add(&byte, sizeof(byte));
    }

    void add(int64_t value)
    {
        add(reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    }

    void add(std::string_view value)
    {
        int64_t len = static_cast<int64_t>(value.size());
        add(len);
        add(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    void add(const std::string& value)
    {
        add(std::string_view(value));
    }

    void add(const uint8_t* data, size_t size)
    {
        for (size_t i = 0; i < size; ++i) {
            hashv ^= data[i];
            hashv *= 16777619u;
        }
    }
    template<typename T>
    void add(T) = delete;
};

struct CommonStruct {
    struct PropertyCommon {
        std::string_view m_name;
        std::string      getName() const { return std::string(m_name); }
    };
    template<typename Scalar, typename Derived>
    struct PropertyScalar : public PropertyCommon {
        Scalar* m_pointer = nullptr;
        Scalar  m_zero{};
        Scalar  m_recc{};

        struct Preset {
            int    m_globalIndex = 0;
            Scalar m_value;
        };
        std::vector<Preset> m_presets;
        bool                m_isConstant  = false;
        bool                m_skipPresets = false;

        void calchash(ConfigHasher& hasher) const
        {
            hasher.add(*m_pointer);
        }

        Derived& addPreset(int globalIndex, Scalar value)
        {
            Preset preset;
            preset.m_globalIndex = globalIndex;
            preset.m_value       = value;
            m_presets.push_back(std::move(preset));
            return static_cast<Derived&>(*this);
        }
        Derived& setParent(CommonStruct* parent)
        {
            m_parent = parent;
            return static_cast<Derived&>(*this);
        }
        Derived& setZero(Scalar value)
        {
            m_zero = value;
            m_recc = value;
            return static_cast<Derived&>(*this);
        }
        Derived& setRecc(Scalar value)
        {
            m_recc = value;
            return static_cast<Derived&>(*this);
        }
        Derived& setConstant()
        {
            m_isConstant = true;
            return static_cast<Derived&>(*this);
        }
        Derived& skipPresets()
        {
            m_skipPresets = true;
            return static_cast<Derived&>(*this);
        }

        CommonStruct* m_parent = nullptr;
        // this is to send notification from BL to UX, e.g. "we reloaded configuration, please update controls"
        std::function<void()> m_sendChangeToInner = [] {};

        void notifyChangeInner()
        {
            if (!m_isConstant)
                m_sendChangeToInner();
        }
        void notifyChangeOuter()
        {
            if (!m_isConstant && m_parent)
                return m_parent->notifyChangeOuter();
        }
        // this is set in UX to subscribe for changes from BL
        void setCallbackForOuterChanges(std::function<void()>&& callback)
        {
            m_sendChangeToInner = std::move(callback);
        }

        Scalar get() const { return *m_pointer; }
        void   set(const Scalar& val)
        {
            if (!m_isConstant)
                *m_pointer = val;
        }
        void resetToZero()
        {
            if (m_skipPresets || m_isConstant)
                return;
            *m_pointer = m_zero;
        }
        void resetToRecommended()
        {
            if (m_skipPresets || m_isConstant)
                return;
            *m_pointer = m_recc;
        }

        void activatePreset(int globalIndex)
        {
            if (m_skipPresets || m_isConstant)
                return;
            for (auto& preset : m_presets)
                if (preset.m_globalIndex == globalIndex)
                    set(preset.m_value);
        }
    };

    struct PropertyInt : public PropertyScalar<int64_t, PropertyInt> {
        int64_t m_min         = 0;
        int64_t m_max         = 0;
        bool    m_isPercent   = false;
        bool    m_hasLogScale = false;

        PropertyInt& setRange(int64_t min, int64_t max)
        {
            m_min = min;
            m_max = max;
            return *this;
        }
        PropertyInt& setPercent()
        {
            m_isPercent = true;
            return *this;
        }
        PropertyInt& setLogScale()
        {
            m_hasLogScale = true;
            return *this;
        }
    };
    struct PropertyBool : public PropertyScalar<bool, PropertyBool> {
    };
    struct PropertyString : public PropertyScalar<std::string, PropertyString> {
    };

protected:
    template<typename Property, typename Scalar>
    struct PropertyContainer {
        Property& add(std::string_view name, Scalar& val)
        {
            Property prop;
            prop.m_name    = name;
            prop.m_pointer = &val;
            m_list.push_back(std::move(prop));
            return m_list.back();
        }
        void resetToZero()
        {
            for (auto& prop : m_list)
                prop.resetToZero();
        }
        void resetToRecommended()
        {
            for (auto& prop : m_list)
                prop.resetToRecommended();
        }
        void activatePreset(int globalIndex)
        {
            for (auto& prop : m_list)
                prop.activatePreset(globalIndex);
        }
        void readFromProperty(const Mernel::PropertyTree& propertyTree)
        {
            for (auto& prop : m_list)
                if (!prop.m_isConstant)
                    propertyTree.value(prop.getName(), Mernel::PropertyTreeScalar(prop.m_zero)).convertTo(*prop.m_pointer);
        }

        void writeToProperty(Mernel::PropertyTree& propertyTree) const
        {
            for (auto& prop : m_list) {
                if (*prop.m_pointer == prop.m_zero || prop.m_isConstant)
                    continue;

                propertyTree[prop.getName()] = Mernel::PropertyTreeScalar(*prop.m_pointer);
            }
        }
        void notifyChangeInner()
        {
            for (auto& prop : m_list)
                prop.notifyChangeInner();
        }

        void assign(PropertyContainer& other)
        {
            for (size_t i = 0; i < m_list.size() && i < other.m_list.size(); ++i)
                *(m_list[i].m_pointer) = *(other.m_list[i].m_pointer);
        }

        void calchash(ConfigHasher& hasher) const
        {
            hasher.add(static_cast<int64_t>(m_list.size()));
            for (auto& prop : m_list)
                prop.calchash(hasher);
        }

        std::vector<Property> m_list;
    };

    PropertyContainer<PropertyInt, int64_t>        m_int;
    PropertyContainer<PropertyString, std::string> m_string;
    PropertyContainer<PropertyBool, bool>          m_bool;

    PropertyInt& addScalar(std::string_view name, int64_t& val)
    {
        return m_int.add(name, val).setParent(this);
    }
    PropertyBool& addScalar(std::string_view name, bool& val)
    {
        return m_bool.add(name, val).setParent(this);
    }
    PropertyString& addScalar(std::string_view name, std::string& val)
    {
        return m_string.add(name, val).setParent(this);
    }

    struct Child : public PropertyCommon {
        CommonStruct* m_record;
    };

    std::vector<Child> m_children;

    template<typename Container>
    static void describeContainer(const Container& container, const char* type, Mernel::PropertyTree& list, const std::string& prefix)
    {
        for (const auto& prop : container.m_list) {
            if (prop.m_isConstant)
                continue;
            Mernel::PropertyTree item;
            item.convertToMap();
            item["path"]        = Mernel::PropertyTreeScalar(prefix + prop.getName());
            item["type"]        = Mernel::PropertyTreeScalar(type);
            item["value"]       = Mernel::PropertyTreeScalar(*prop.m_pointer);
            item["default"]     = Mernel::PropertyTreeScalar(prop.m_zero);
            item["recommended"] = Mernel::PropertyTreeScalar(prop.m_recc);
            if constexpr (requires { prop.m_min; }) {
                item["min"] = Mernel::PropertyTreeScalar(prop.m_min);
                item["max"] = Mernel::PropertyTreeScalar(prop.m_max);
                if (prop.m_isPercent)
                    item["percent"] = Mernel::PropertyTreeScalar(true);
                if (prop.m_hasLogScale)
                    item["logScale"] = Mernel::PropertyTreeScalar(true);
            }
            if (prop.m_skipPresets) {
                item["presetsIgnore"] = Mernel::PropertyTreeScalar(true);
            } else if (!prop.m_presets.empty()) {
                Mernel::PropertyTree presets;
                presets.convertToMap();
                for (const auto& preset : prop.m_presets)
                    if (preset.m_globalIndex >= 0 && preset.m_globalIndex < static_cast<int>(s_presetNames.size()))
                        presets[s_presetNames[preset.m_globalIndex]] = Mernel::PropertyTreeScalar(preset.m_value);
                item["presets"] = std::move(presets);
            }
            list.append(std::move(item));
        }
    }

    CommonStruct* m_parent = nullptr;

    // this is to send notification from UX to BL, e.g. "user changed slider, please make undo frame"
    std::function<void()> m_sendChangeToOuter = [] {};

    Child& addStruct(std::string_view name, CommonStruct& s)
    {
        s.m_parent = this;
        Child child;
        child.m_name   = name;
        child.m_record = &s;
        m_children.push_back(std::move(child));
        return m_children.back();
    }

public:
    void calchash(ConfigHasher& hasher) const
    {
        m_bool.calchash(hasher);
        m_int.calchash(hasher);
        m_string.calchash(hasher);
        hasher.add(static_cast<int64_t>(m_children.size()));
        for (auto& prop : m_children)
            prop.m_record->calchash(hasher);
    }

    // this called in BL to send notification to UX
    void notifyChangeInner()
    {
        for (auto& child : m_children) {
            child.m_record->notifyChangeInner();
        }

        m_int.notifyChangeInner();
        m_bool.notifyChangeInner();
        m_string.notifyChangeInner();
    }

    // this called in UX to send notification to BL
    void notifyChangeOuter()
    {
        if (m_parent)
            return m_parent->notifyChangeOuter();

        m_sendChangeToOuter();
    }

    // this is set in BL to subscribe for changes from UX
    void setCallbackForInnerChanges(std::function<void()>&& callback)
    {
        m_sendChangeToOuter = std::move(callback);
    }

    void notifyInnerAndOuter(bool notifyTwoWay)
    {
        if (!notifyTwoWay)
            return;

        notifyChangeInner();
        notifyChangeOuter();
    }

    void resetToZero(bool notifyTwoWay = false)
    {
        m_int.resetToZero();
        m_bool.resetToZero();
        m_string.resetToZero();
        for (auto& child : m_children)
            child.m_record->resetToZero(false);

        notifyInnerAndOuter(notifyTwoWay);
    }
    void resetToRecommended(bool notifyTwoWay = false)
    {
        m_int.resetToRecommended();
        m_bool.resetToRecommended();
        m_string.resetToRecommended();
        for (auto& child : m_children)
            child.m_record->resetToRecommended();

        notifyInnerAndOuter(notifyTwoWay);
    }

    void assign(CommonStruct& another, bool notifyTwoWay = false)
    {
        m_int.assign(another.m_int);
        m_bool.assign(another.m_bool);
        m_string.assign(another.m_string);
        for (size_t i = 0, cnt = std::min(m_children.size(), another.m_children.size()); i < cnt; ++i)
            m_children[i].m_record->assign(*(another.m_children[i].m_record));

        notifyInnerAndOuter(notifyTwoWay);
    }

    void activatePreset(int globalIndex, bool notifyTwoWay = false)
    {
        m_int.activatePreset(globalIndex);
        m_bool.activatePreset(globalIndex);
        m_string.activatePreset(globalIndex);
        for (auto& child : m_children)
            child.m_record->activatePreset(globalIndex);

        notifyInnerAndOuter(notifyTwoWay);
    }

    void validateAndClamp()
    {
        for (auto& prop : m_int.m_list)
            *prop.m_pointer = std::clamp(*prop.m_pointer, prop.m_min, prop.m_max);
        for (auto& child : m_children) {
            child.m_record->validateAndClamp();
        }
    }

    void readFromPropertyImpl(const Mernel::PropertyTree& propertyTree)
    {
        m_int.readFromProperty(propertyTree);
        m_bool.readFromProperty(propertyTree);
        m_string.readFromProperty(propertyTree);

        for (auto& child : m_children) {
            const std::string key = child.getName();
            if (!propertyTree.contains(key)) {
                continue;
            }
            child.m_record->readFromPropertyImpl(propertyTree[key]);
        }
    }

    void readFromProperty(const Mernel::PropertyTree& propertyTree)
    {
        resetToZero();
        readFromPropertyImpl(propertyTree);
        validateAndClamp();
    }

    void writeToProperty(Mernel::PropertyTree& propertyTree) const
    {
        propertyTree = {};
        m_int.writeToProperty(propertyTree);
        m_bool.writeToProperty(propertyTree);
        m_string.writeToProperty(propertyTree);

        for (auto& child : m_children) {
            Mernel::PropertyTree childData;
            child.m_record->writeToProperty(childData);
            if (childData.isNull())
                continue;

            propertyTree[child.getName()] = std::move(childData);
        }
    }

    // Reflection for tools (the console app's agent commands): appends one map per setting under this
    // struct to `list`: path ("balanceMonsters.monsterAttack"), type, value, default, recommended,
    // presets and, for integers, min/max/percent/logScale. Constant settings are left out.
    void describeSettings(Mernel::PropertyTree& list, const std::string& prefix = {}) const
    {
        if (!list.isList())
            list.convertToList();
        describeContainer(m_bool, "bool", list, prefix);
        describeContainer(m_int, "int", list, prefix);
        describeContainer(m_string, "string", list, prefix);
        for (auto& child : m_children)
            child.m_record->describeSettings(list, prefix + child.getName() + ".");
    }

    // Sets one setting by its dotted path from text ("150", "true", "abc"). Returns an empty string on
    // success, otherwise what is wrong (unknown name, not a number, outside min..max).
    std::string setByPath(std::string_view path, std::string_view text)
    {
        const auto dot = path.find('.');
        if (dot != std::string_view::npos) {
            const auto head = path.substr(0, dot);
            for (auto& child : m_children)
                if (child.m_name == head)
                    return child.m_record->setByPath(path.substr(dot + 1), text);
            return "no section '" + std::string(head) + "'";
        }
        for (auto& prop : m_int.m_list) {
            if (prop.m_name != path || prop.m_isConstant)
                continue;
            int64_t value  = 0;
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc() || end != text.data() + text.size())
                return "'" + std::string(path) + "' needs a whole number, got '" + std::string(text) + "'";
            if (value < prop.m_min || value > prop.m_max)
                return "'" + std::string(path) + "' must be " + std::to_string(prop.m_min) + ".." + std::to_string(prop.m_max) + ", got " + std::to_string(value);
            prop.set(value);
            return {};
        }
        for (auto& prop : m_bool.m_list) {
            if (prop.m_name != path || prop.m_isConstant)
                continue;
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
                prop.set(true);
            else if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
                prop.set(false);
            else
                return "'" + std::string(path) + "' needs true or false, got '" + std::string(text) + "'";
            return {};
        }
        for (auto& prop : m_string.m_list) {
            if (prop.m_name != path || prop.m_isConstant)
                continue;
            prop.set(std::string(text));
            return {};
        }
        return "no setting '" + std::string(path) + "'";
    }

    PropertyInt& getPropertyInt(size_t index)
    {
        return m_int.m_list[index];
    }
    PropertyBool& getPropertyBool(size_t index)
    {
        return m_bool.m_list[index];
    }
    PropertyString& getPropertyString(size_t index)
    {
        return m_string.m_list[index];
    }

    static inline std::vector<std::string> s_presetNames{};

    static bool setPresetNames(std::vector<std::string> names)
    {
        s_presetNames = std::move(names);
        return true;
    }
    static int getPresetIndex(const std::string& name)
    {
        auto it = std::find(s_presetNames.cbegin(), s_presetNames.cend(), name);
        if (it == s_presetNames.cend())
            return -1;
        return static_cast<int>(std::distance(s_presetNames.cbegin(), it));
    }
};

}
