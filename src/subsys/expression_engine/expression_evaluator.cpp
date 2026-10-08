#include <span>
#include <utility>

#include "expression_evaluator.h"

namespace eerie_leap::subsys::expression_engine {

ExpressionEvaluator::ExpressionEvaluator(
    std::pmr::memory_resource* resource, std::string_view expression, ee::Program program)
        : expression_(expression, resource),
        program_(std::move(program)),
        input_index_(program_.VariableIndex(k_input_name)) { }

std::expected<ExpressionEvaluator, CompileError> ExpressionEvaluator::Create(
    std::string_view expression, std::pmr::memory_resource* resource) noexcept {

    auto program = ee::Compile(expression, resource, k_limits);
    if(!program)
        return std::unexpected(program.error());

    return ExpressionEvaluator(resource, expression, std::move(*program));
}

std::string ExpressionEvaluator::Describe(const CompileError& error) {
    return "Invalid expression: " + std::string(ee::Describe(error.code))
        + " at " + std::to_string(static_cast<unsigned>(error.position)) + ".";
}

bool ExpressionEvaluator::BindVariable(std::string_view name, const float* value) noexcept {
    if(value == nullptr || name == k_input_name)
        return false;

    const auto index = program_.VariableIndex(name);
    if(!index.has_value())
        return false;

    bindings_[*index] = value;

    return true;
}

std::optional<float> ExpressionEvaluator::Evaluate(std::optional<float> input) const noexcept {
    std::array<float, ee::limits::kMaxVariables> values;
    const size_t count = program_.VariableCount();

    for(size_t i = 0; i < count; i++) {
        if(input_index_ == i) {
            if(!input.has_value())
                return std::nullopt;

            values[i] = *input;
        } else {
            if(bindings_[i] == nullptr)
                return std::nullopt;

            values[i] = *bindings_[i];
        }
    }

    return program_.Evaluate(std::span<const float>(values.data(), count));
}

} // namespace eerie_leap::subsys::expression_engine
