#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <eerie_leap/expression_engine/expression_engine.hpp>

namespace eerie_leap::subsys::expression_engine {

namespace ee = ::eerie_leap::expression_engine;

using ee::CompileError;

// A sensor expression compiled once and evaluated per sample without allocating.
//
// Variables keep the engine's numbering (order of first appearance). "x" is the sensor's own
// input and is supplied to Evaluate(); every other name is a sensor ID whose value address is
// bound once with BindVariable(). Neither direction throws.
class ExpressionEvaluator {
private:
    static constexpr std::string_view k_input_name = "x";

    // The configuration work queue has a small stack; Compile() needs ~1 kB per nesting level.
    static constexpr ee::Limits k_limits { .max_nesting = 8 };

    std::pmr::string expression_;
    ee::Program program_;
    std::optional<size_t> input_index_;
    std::array<const float*, ee::limits::kMaxVariables> bindings_ { };

    ExpressionEvaluator(std::pmr::memory_resource* resource, std::string_view expression, ee::Program program);

public:
    ExpressionEvaluator(ExpressionEvaluator&&) noexcept = default;
    ExpressionEvaluator& operator=(ExpressionEvaluator&&) noexcept = default;
    ExpressionEvaluator(const ExpressionEvaluator&) = delete;
    ExpressionEvaluator& operator=(const ExpressionEvaluator&) = delete;

    // Stores the text and the program on @p resource.
    static std::expected<ExpressionEvaluator, CompileError> Create(
        std::string_view expression, std::pmr::memory_resource* resource) noexcept;

    // "Invalid expression: <what> at <position>", for validation messages.
    static std::string Describe(const CompileError& error);

    const std::pmr::string& GetExpression() const noexcept { return expression_; }

    // Every variable the expression reads, in program order, including x.
    auto GetAllVariableNames() const noexcept { return program_.VariableNames(); }

    // The sensor IDs the expression reads, in program order; x is excluded.
    auto GetVariableNames() const noexcept {
        return program_.VariableNames()
            | std::views::filter([](std::string_view name) { return name != k_input_name; });
    }

    size_t GetVariableCount() const noexcept { return program_.VariableCount(); }
    bool UsesInput() const noexcept { return input_index_.has_value(); }

    // Reads @p value on every evaluation; the address must stay valid while the evaluator is used.
    // False for a name the expression does not read, for x, or for a null address.
    bool BindVariable(std::string_view name, const float* value) noexcept;

    // Nothing when a sensor variable is unbound or the expression needs x and none is given.
    // NaN propagates from the program, so the caller decides what it means.
    std::optional<float> Evaluate(std::optional<float> input = std::nullopt) const noexcept;
};

} // namespace eerie_leap::subsys::expression_engine
