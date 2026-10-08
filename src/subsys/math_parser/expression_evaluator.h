#pragma once

#include <memory>
#include <string>
#include <optional>
#include <unordered_set>

#include "math_parser.h"

namespace eerie_leap::subsys::math_parser {

class ExpressionEvaluator {
private:
    std::unique_ptr<MathParser> math_parser_;
    std::string expression_;
    float x_;
    bool uses_x_ = false;

public:
    // Throws std::invalid_argument for an expression that does not parse.
    explicit ExpressionEvaluator(std::string expression);
    virtual ~ExpressionEvaluator() = default;

    ExpressionEvaluator(const ExpressionEvaluator&) = delete;
    ExpressionEvaluator& operator=(const ExpressionEvaluator&) = delete;

    const std::string& GetExpression() const;
    const std::unordered_set<std::string>& GetVariableNames() const;
    void RegisterVariableValueHandler(const MathParser::VariableFactoryHandler& handler);

    // Reads the variable from @p value on every evaluation; the address must outlive the evaluator's use.
    void BindVariable(const std::string& name, float* value);

    // Throws std::invalid_argument when the expression uses x and none is given,
    // std::runtime_error when the parser fails.
    float Evaluate(std::optional<float> x = std::nullopt);
};

} // namespace eerie_leap::subsys::math_parser
