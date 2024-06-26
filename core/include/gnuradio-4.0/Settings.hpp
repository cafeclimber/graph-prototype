#ifndef GNURADIO_SETTINGS_HPP
#define GNURADIO_SETTINGS_HPP

#include <atomic>
#include <chrono>
#include <concepts>
#include <mutex>
#include <optional>
#include <set>
#include <variant>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <gnuradio-4.0/annotated.hpp>
#include <gnuradio-4.0/BlockTraits.hpp>
#include <gnuradio-4.0/meta/formatter.hpp>
#include <gnuradio-4.0/reflection.hpp>
#include <gnuradio-4.0/Tag.hpp>

namespace gr {

namespace settings {
template<typename T>
inline constexpr static bool
isSupportedType() {
    return std::is_arithmetic_v<T> || std::is_same_v<T, std::string> || gr::meta::vector_type<T> || std::is_same_v<T, property_map>;
}
} // namespace settings

struct ApplyStagedParametersResult {
    property_map forwardParameters; // parameters that should be forwarded to dependent child blocks
    property_map appliedParameters;
};

namespace detail {
template<class T>
inline constexpr void
hash_combine(std::size_t &seed, const T &v) noexcept {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}
} // namespace detail

struct SettingsCtx {
    // using TimePoint = std::chrono::time_point<std::chrono::utc_clock>; // TODO: change once the C++20 support is ubiquitous
    using TimePoint               = std::chrono::time_point<std::chrono::system_clock>;
    std::optional<TimePoint> time = std::nullopt; /// UTC time-stamp from which the setting is valid
    property_map             context;             /// user-defined multiplexing context for which the setting is valid

    SettingsCtx() {}

    explicit SettingsCtx(const TimePoint &t, const property_map &ctx = {}) {
        time    = t;
        context = ctx;
    }

    bool
    operator==(const SettingsCtx &) const
            = default;

    bool
    operator<(const SettingsCtx &other) {
        // order by time
        return !time || (other.time && *time < *other.time);
    }

    [[nodiscard]] std::size_t
    hash() const noexcept {
        std::size_t seed = 0;
        if (time) {
            detail::hash_combine(seed, time.value().time_since_epoch().count());
        }
        for (const auto &[key, val] : context) {
            detail::hash_combine(seed, key);
            detail::hash_combine(seed, pmtv::to_base64(val));
        }
        return seed;
    }
};

/**
 * @brief a concept verifying whether a processing block optionally provides a `settingsChanged` callback to react to
 * block configuration changes and/or to influence forwarded downstream parameters.
 *
 * Implementers may have:
 * 1. `settingsChanged(oldSettings, newSettings)`
 * 2. `settingsChanged(oldSettings, newSettings, forwardSettings)`
 *    - where `forwardSettings` is for influencing subsequent blocks. E.g., a decimating block might adjust the `sample_rate` for downstream blocks.
 */
template<typename BlockType>
concept HasSettingsChangedCallback = requires(BlockType *block, const property_map &oldSettings, property_map &newSettings) {
    { block->settingsChanged(oldSettings, newSettings) };
} or requires(BlockType *block, const property_map &oldSettings, property_map &newSettings, property_map &forwardSettings) {
    { block->settingsChanged(oldSettings, newSettings, forwardSettings) };
};

/**
 * @brief a concept verifying whether a processing block optionally provides a `reset` callback to react to
 * block reset requests (being called after the settings have been reverted(.
 */
template<typename TBlock>
concept HasSettingsResetCallback = requires(TBlock *block) {
    { block->reset() };
};

template<typename T>
concept SettingsLike = requires(T t, std::span<const std::string> parameter_keys, const std::string &parameter_key, const property_map &parameters, SettingsCtx ctx) {
    /**
     * @brief returns if there are stages settings that haven't been applied yet.
     */
    { t.changed() } -> std::same_as<bool>;

    /**
     * @brief stages new key-value pairs that shall replace the block field-based settings.
     * N.B. settings become only active after executing 'applyStagedParameters()' (usually done early on in the 'Block::work()' function)
     * @return key-value pairs that could not be set
     */
    { t.set(parameters, ctx) } -> std::same_as<property_map>;
    { t.set(parameters) } -> std::same_as<property_map>;

    /**
     * @brief updates parameters based on block input tags for those with keys stored in `autoUpdateParameters()`
     * Parameter changes to down-stream blocks is controlled via `autoForwardParameters()`
     */
    { t.autoUpdate(parameters, ctx) } -> std::same_as<void>;
    { t.autoUpdate(parameters) } -> std::same_as<void>;

    /**
     * @brief return all available block settings as key-value pairs
     */
    { t.get() } -> std::same_as<property_map>;

    /**
     * @brief return key-pmt values map for multiple keys
     */
    { t.get(parameter_keys, ctx) } -> std::same_as<property_map>;
    { t.get(parameter_keys) } -> std::same_as<property_map>;

    /**
     * @brief return pmt value for a single key
     */
    { t.get(parameter_key, ctx) } -> std::same_as<std::optional<pmtv::pmt>>;
    { t.get(parameter_key) } -> std::same_as<std::optional<pmtv::pmt>>;

    /**
     * @brief returns the staged/not-yet-applied new parameters
     */
    { t.stagedParameters() } -> std::same_as<const property_map>;

    /**
     * @brief synchronise map-based with actual block field-based settings
     */
    { t.applyStagedParameters() } -> std::same_as<ApplyStagedParametersResult>;

    /**
     * @brief synchronises the map-based with the block's field-based parameters
     * (N.B. usually called after the staged parameters have been synchronised)
     */
    { t.updateActiveParameters() } -> std::same_as<void>;
};

struct SettingsBase {
    std::atomic_bool _changed{ false };

    virtual ~SettingsBase() = default;

    void
    swap(SettingsBase &other) noexcept {
        if (this == &other) {
            return;
        }
        bool temp = _changed;
        // avoid CAS-loop since this called only during initialisation where there is no concurrent access possible.
        std::atomic_store_explicit(&_changed, std::atomic_load_explicit(&other._changed, std::memory_order_acquire), std::memory_order_release);
        other._changed = temp;
    }

    /**
     * @brief returns if there are stages settings that haven't been applied yet.
     */
    [[nodiscard]] bool
    changed() const noexcept {
        return _changed;
    }

    /**
     * @brief stages new key-value pairs that shall replace the block field-based settings.
     * N.B. settings become only active after executing 'applyStagedParameters()' (usually done early on in the 'Block::work()' function)
     * @return key-value pairs that could not be set
     */
    [[nodiscard]] virtual property_map
    set(const property_map &parameters, SettingsCtx ctx = {})
            = 0;

    virtual void
    storeDefaults()
            = 0;
    virtual void
    resetDefaults()
            = 0;

    /**
     * @brief updates parameters based on block input tags for those with keys stored in `autoUpdateParameters()`
     * Parameter changes to down-stream blocks is controlled via `autoForwardParameters()`
     */
    virtual void
    autoUpdate(const property_map &parameters, SettingsCtx = {})
            = 0;

    /**
     * @brief return all (or for selected multiple keys) available block settings as key-value pairs
     */
    [[nodiscard]] virtual property_map
    get(std::span<const std::string> parameter_keys = {}, SettingsCtx = {}) const noexcept
            = 0;

    [[nodiscard]] virtual std::optional<pmtv::pmt>
    get(const std::string &parameter_key, SettingsCtx = {}) const noexcept = 0;

    /**
     * @brief returns the staged/not-yet-applied new parameters
     */
    [[nodiscard]] virtual const property_map
    stagedParameters() const
            = 0;

    [[nodiscard]] virtual std::set<std::string, std::less<>> &
    autoUpdateParameters() noexcept
            = 0;

    [[nodiscard]] virtual std::set<std::string, std::less<>> &
    autoForwardParameters() noexcept
            = 0;

    /**
     * @brief synchronise map-based with actual block field-based settings
     * returns map with key-value tags that should be forwarded
     * to dependent/child blocks.
     */
    [[nodiscard]] virtual ApplyStagedParametersResult
    applyStagedParameters()
            = 0;

    /**
     * @brief synchronises the map-based with the block's field-based parameters
     * (N.B. usually called after the staged parameters have been synchronised)
     */
    virtual void
    updateActiveParameters() noexcept
            = 0;
};

namespace detail {
template<typename T>
concept HasBaseType = requires { typename std::remove_cvref_t<T>::base_t; };
}; // namespace detail

template<typename TBlock>
class BasicSettings : public SettingsBase {
    TBlock                            *_block = nullptr;
    mutable std::mutex                 _lock{};
    property_map                       _active{}; // copy of class field settings as pmt-style map
    property_map                       _staged{}; // parameters to become active before the next work() call
    std::set<std::string, std::less<>> _auto_update{};
    std::set<std::string, std::less<>> _auto_forward{};
    property_map                       _default_settings{};

public:
    BasicSettings()  = delete;
    ~BasicSettings() = default;

    explicit constexpr BasicSettings(TBlock &block) noexcept : SettingsBase(), _block(&block) {
        if constexpr (requires { &TBlock::settingsChanged; }) { // if settingsChanged is defined
            static_assert(HasSettingsChangedCallback<TBlock>, "if provided, settingsChanged must have either a `(const property_map& old, property_map& new, property_map& fwd)`"
                                                              "or `(const property_map& old, property_map& new)` parameter signatures.");
        }

        if constexpr (requires { &TBlock::reset; }) { // if reset is defined
            static_assert(HasSettingsResetCallback<TBlock>, "if provided, reset() may have no function parameters");
        }

        if constexpr (refl::is_reflectable<TBlock>()) {
            // register block-global description
            constexpr bool hasMetaInfo = requires(TBlock t) {
                {
                    unwrap_if_wrapped_t<decltype(t.meta_information)> {}
                } -> std::same_as<property_map>;
            };

            if constexpr (hasMetaInfo && requires(TBlock t) { t.description; }) {
                static_assert(std::is_same_v<std::remove_cvref_t<unwrap_if_wrapped_t<decltype(TBlock::description)>>, std::string_view>);
                _block->meta_information.value["description"] = std::string(_block->description);
            }

            // handle meta-information for UI and other non-processing-related purposes
            auto iterate_over_member = [this]<typename Member>(Member member) {
                using RawType         = std::remove_cvref_t<decltype(member(*_block))>;
                using Type            = unwrap_if_wrapped_t<RawType>;
                const auto memberName = std::string(get_display_name(member));

                if constexpr (hasMetaInfo && AnnotatedType<RawType>) {
                    _block->meta_information.value[memberName + "::description"]   = std::string(RawType::description());
                    _block->meta_information.value[memberName + "::documentation"] = std::string(RawType::documentation());
                    _block->meta_information.value[memberName + "::unit"]          = std::string(RawType::unit());
                    _block->meta_information.value[memberName + "::visible"]       = RawType::visible();
                }

                // detect whether field has one of the DEFAULT_TAGS signature
                if constexpr (traits::port::is_not_any_port_or_collection<Type> && !std::is_const_v<Type> && is_writable(member) && settings::isSupportedType<Type>()) {
                    meta::tuple_for_each(
                            [&memberName, this](auto &&default_tag) {
                                if (default_tag.shortKey() == memberName) {
                                    _auto_forward.emplace(memberName);
                                }
                            },
                            gr::tag::DEFAULT_TAGS);
                    _auto_update.emplace(memberName);
                }
            };
            processMembers<TBlock>(iterate_over_member);
        }
    }

    constexpr BasicSettings(const BasicSettings &other) noexcept : SettingsBase(other) {
        BasicSettings temp(other);
        swap(temp);
    }

    constexpr BasicSettings(BasicSettings &&other) noexcept : SettingsBase(std::move(other)) {
        BasicSettings temp(std::move(other));
        swap(temp);
    }

    BasicSettings &
    operator=(const BasicSettings &other) noexcept {
        swap(other);
        return *this;
    }

    BasicSettings &
    operator=(BasicSettings &&other) noexcept {
        BasicSettings temp(std::move(other));
        swap(temp);
        return *this;
    }

    void
    swap(BasicSettings &other) noexcept {
        if (this == &other) {
            return;
        }
        SettingsBase::swap(other);
        std::swap(_block, other._block);
        std::scoped_lock lock(_lock, other._lock);
        std::swap(_active, other._active);
        std::swap(_staged, other._staged);
        std::swap(_auto_update, other._auto_update);
        std::swap(_auto_forward, other._auto_forward);
    }

    [[nodiscard]] property_map
    set(const property_map &parameters, SettingsCtx = {}) override {
        property_map ret;
        if constexpr (refl::is_reflectable<TBlock>()) {
            std::lock_guard lg(_lock);
            for (const auto &[key, value] : parameters) {
                bool is_set              = false;
                auto iterate_over_member = [&, this](auto member) {
                    using Type = unwrap_if_wrapped_t<std::remove_cvref_t<decltype(member(*_block))>>;
                    if constexpr (traits::port::is_not_any_port_or_collection<Type> && !std::is_const_v<Type> && is_writable(member) && settings::isSupportedType<Type>()) {
                        const auto fieldName = std::string_view(get_display_name(member));
                        if (fieldName == key && std::holds_alternative<Type>(value)) {
                            if (_auto_update.contains(key)) {
                                _auto_update.erase(key);
                            }
                            _staged.insert_or_assign(key, value);
                            SettingsBase::_changed.store(true);
                            is_set = true;
                        }
                        if (fieldName == key && !std::holds_alternative<Type>(value)) {
                            throw std::invalid_argument([&key, &value] { // lazy evaluation
                                const std::size_t actual_index   = value.index();
                                const std::size_t required_index = meta::to_typelist<pmtv::pmt>::index_of<Type>(); // This too, as per your implementation.
                                return fmt::format("value for key '{}' has a wrong type. Index of actual type: {} ({}), Index of expected type: {} ({})", key, actual_index, "<missing pmt type>",
                                                   required_index, gr::meta::type_name<Type>());
                            }());
                        }
                    }
                };
                processMembers<TBlock>(iterate_over_member);
                if (!is_set) {
                    ret.insert_or_assign(key, pmtv::pmt(value));
                }
            }
        }

        // copy items that could not be matched to the block's meta_information map (if available)
        if constexpr (requires(TBlock t) {
                          {
                              unwrap_if_wrapped_t<decltype(t.meta_information)> {}
                          } -> std::same_as<property_map>;
                      }) {
            updateMaps(ret, _block->meta_information);
        }

        return ret; // N.B. returns those <key:value> parameters that could not be set
    }

    void
    storeDefaults() override {
        this->storeDefaultSettings(_default_settings);
    }

    void
    resetDefaults() override {
        _staged     = _default_settings;
        std::ignore = applyStagedParameters();
        if constexpr (HasSettingsResetCallback<TBlock>) {
            _block->reset();
        }
    }

    void
    autoUpdate(const property_map &parameters, SettingsCtx = {}) override {
        if constexpr (refl::is_reflectable<TBlock>()) {
            for (const auto &[key, value] : parameters) {
                auto iterate_over_member = [&](auto member) {
                    using Type = unwrap_if_wrapped_t<std::remove_cvref_t<decltype(member(*_block))>>;
                    if constexpr (traits::port::is_not_any_port_or_collection<Type> && !std::is_const_v<Type> && is_writable(member) && settings::isSupportedType<Type>()) {
                        if (_auto_update.contains(key) && std::string(get_display_name(member)) == key && std::holds_alternative<Type>(value)) {
                            _staged.insert_or_assign(key, value);
                            SettingsBase::_changed.store(true);
                        }
                    }
                };
                processMembers<TBlock>(iterate_over_member);
            }
        }
    }

    [[nodiscard]] const property_map
    stagedParameters() const noexcept override {
        std::lock_guard lg(_lock);
        return _staged;
    }

    [[nodiscard]] property_map
    get(std::span<const std::string> parameter_keys = {}, SettingsCtx = {}) const noexcept override {
        std::lock_guard lg(_lock);
        if (parameter_keys.empty()) {
            return _active;
        }
        property_map ret;
        for (const auto &key : parameter_keys) {
            if (_active.contains(key)) {
                ret.insert_or_assign(key, _active.at(key));
            }
        }
        return ret;
    }

    [[nodiscard]] std::optional<pmtv::pmt>
    get(const std::string &parameter_key, SettingsCtx = {}) const noexcept override {
        if constexpr (refl::is_reflectable<TBlock>()) {
            std::lock_guard lg(_lock);

            if (_active.contains(parameter_key)) {
                return { _active.at(parameter_key) };
            }
        }

        return std::nullopt;
    }

    [[nodiscard]] std::set<std::string, std::less<>> &
    autoUpdateParameters() noexcept override {
        return _auto_update;
    }

    [[nodiscard]] std::set<std::string, std::less<>> &
    autoForwardParameters() noexcept override {
        return _auto_forward;
    }

    /**
     * @brief synchronise map-based with actual block field-based settings
     * returns a structure containing three maps:
     *  - forwardParameters -- map with key-value tags that should be forwarded
     *    to dependent/child blocks.
     *  - appliedParameters -- map with peoperties that were successfully set
     */
    [[nodiscard]] ApplyStagedParametersResult
    applyStagedParameters() override {
        ApplyStagedParametersResult result;
        if constexpr (refl::is_reflectable<TBlock>()) {
            std::lock_guard lg(_lock);

            // prepare old settings if required
            property_map oldSettings;
            if constexpr (HasSettingsChangedCallback<TBlock>) {
                storeDefaultSettings(oldSettings);
            }

            // check if reset of settings should be performed
            if (_staged.contains(gr::tag::RESET_DEFAULTS)) {
                _staged.clear();
                resetDefaults();
            }

            // update staged and forward parameters based on member properties
            property_map staged;
            for (const auto &[localKey, localStaged_value] : _staged) {
                const auto &key                  = localKey;
                const auto &staged_value         = localStaged_value;
                auto        apply_member_changes = [&key, &staged, &result, &staged_value, this](auto member) {
                    using RawType = std::remove_cvref_t<decltype(member(*_block))>;
                    using Type    = unwrap_if_wrapped_t<RawType>;
                    if constexpr (traits::port::is_not_any_port_or_collection<Type> && !std::is_const_v<Type> && is_writable(member) && settings::isSupportedType<Type>()) {
                        if (std::string(get_display_name(member)) == key && std::holds_alternative<Type>(staged_value)) {
                            if constexpr (is_annotated<RawType>()) {
                                if (member(*_block).validate_and_set(std::get<Type>(staged_value))) {
                                    result.appliedParameters.insert_or_assign(key, staged_value);
                                    if constexpr (HasSettingsChangedCallback<TBlock>) {
                                        staged.insert_or_assign(key, staged_value);
                                    } else {
                                        std::ignore = staged; // help clang to see why staged is not unused
                                    }
                                } else {
                                    // TODO: replace with pmt error message on msgOut port (to note: clang compiler bug/issue)
#if !defined(__EMSCRIPTEN__) && !defined(__clang__)
                                    fmt::print(stderr, " cannot set field {}({})::{} = {} to {} due to limit constraints [{}, {}] validate func is {} defined\n", //
                                               _block->unique_name, _block->name, member(*_block), std::get<Type>(staged_value),                                  //
                                               std::string(get_display_name(member)), RawType::LimitType::MinRange,
                                               RawType::LimitType::MaxRange, //
                                               RawType::LimitType::ValidatorFunc == nullptr ? "not" : "");
#else
                                    fmt::print(stderr, " cannot set field {}({})::{} = {} to {} due to limit constraints [{}, {}] validate func is {} defined\n", //
                                               "_block->unique_name", "_block->name", "member(*_block)", std::get<Type>(staged_value),                            //
                                               std::string(get_display_name(member)), RawType::LimitType::MinRange,
                                               RawType::LimitType::MaxRange, //
                                               RawType::LimitType::ValidatorFunc == nullptr ? "not" : "");
#endif
                                }
                            } else {
                                member(*_block) = std::get<Type>(staged_value);
                                result.appliedParameters.insert_or_assign(key, staged_value);
                                if constexpr (HasSettingsChangedCallback<TBlock>) {
                                    staged.insert_or_assign(key, staged_value);
                                } else {
                                    std::ignore = staged; // help clang to see why staged is not unused
                                }
                            }
                        }
                        if (_auto_forward.contains(key)) {
                            result.forwardParameters.insert_or_assign(key, staged_value);
                        }
                    }
                };
                processMembers<TBlock>(apply_member_changes);
            }

            // update active parameters
            auto update_active = [this](auto member) {
                using Type = unwrap_if_wrapped_t<std::remove_cvref_t<decltype(member(*_block))>>;
                if constexpr (traits::port::is_not_any_port_or_collection<Type> && is_readable(member) && settings::isSupportedType<Type>()) {
                    _active.insert_or_assign(get_display_name(member), static_cast<Type>(member(*_block)));
                }
            };
            processMembers<TBlock>(update_active);

            // invoke user-callback function if staged is not empty
            if (!staged.empty()) {
                if constexpr (requires { _block->settingsChanged(/* old settings */ _active, /* new settings */ staged); }) {
                    _block->settingsChanged(/* old settings */ oldSettings, /* new settings */ staged);
                } else if constexpr (requires { _block->settingsChanged(/* old settings */ _active, /* new settings */ staged, /* new forward settings */ result.forwardParameters); }) {
                    _block->settingsChanged(/* old settings */ oldSettings, /* new settings */ staged, /* new forward settings */ result.forwardParameters);
                }
            }

            if (_staged.contains(gr::tag::STORE_DEFAULTS)) {
                storeDefaults();
            }

            if constexpr (HasSettingsResetCallback<TBlock>) {
                if (_staged.contains(gr::tag::RESET_DEFAULTS)) {
                    _block->reset();
                }
            }

            _staged.clear();
        }

        SettingsBase::_changed.store(false);
        return result;
    }

    void
    updateActiveParameters() noexcept override {
        if constexpr (refl::is_reflectable<TBlock>()) {
            std::lock_guard lg(_lock);
            auto            iterate_over_member = [&, this](auto member) {
                using Type = unwrap_if_wrapped_t<std::remove_cvref_t<decltype(member(*_block))>>;
                if constexpr (traits::port::is_not_any_port_or_collection<Type> && is_readable(member) && settings::isSupportedType<Type>()) {
                    _active.insert_or_assign(get_display_name(member), static_cast<Type>(member(*_block)));
                }
            };
            processMembers<TBlock>(iterate_over_member);
        }
    }

private:
    void
    storeDefaultSettings(property_map &oldSettings) {
        // take a copy of the field -> map value of the old settings
        if constexpr (refl::is_reflectable<TBlock>()) {
            auto iterate_over_member = [&, this](auto member) {
                using Type = unwrap_if_wrapped_t<std::remove_cvref_t<decltype(member(*_block))>>;

                if constexpr (traits::port::is_not_any_port_or_collection<Type> && is_readable(member) && settings::isSupportedType<Type>()) {
                    oldSettings.insert_or_assign(get_display_name(member), pmtv::pmt(member(*_block)));
                }
            };
            if constexpr (detail::HasBaseType<TBlock>) {
                refl::util::for_each(refl::reflect<typename std::remove_cvref_t<TBlock>::base_t>().members, iterate_over_member);
            }
            refl::util::for_each(refl::reflect<TBlock>().members, iterate_over_member);
        }
    }

    template<typename T, typename Func>
    inline constexpr static void
    processMembers(Func func) {
        if constexpr (detail::HasBaseType<T>) {
            refl::util::for_each(refl::reflect<typename std::remove_cvref_t<T>::base_t>().members, func);
        }
        refl::util::for_each(refl::reflect<T>().members, func);
    }
};

static_assert(SettingsLike<BasicSettings<int>>);

} // namespace gr

namespace std {
template<>
struct hash<gr::SettingsCtx> {
    [[nodiscard]] size_t
    operator()(const gr::SettingsCtx &ctx) const noexcept {
        return ctx.hash();
    }
};
} // namespace std

#endif // GNURADIO_SETTINGS_HPP
