#include <stdexcept>
#include <string>

#include "expression_evaluator.h"

namespace eerie_leap::subsys::math_parser {

using namespace mu;

ExpressionEvaluator::ExpressionEvaluator(std::string expression)
    : expression_(std::move(expression)), x_(0.0) {

    // mu::ParserError is not a std::exception, so nothing above would catch it.
    try {
        math_parser_ = std::make_unique<MathParser>(expression_);
    } catch(const ParserError& e) {
        throw std::invalid_argument("Invalid expression: " + e.GetMsg());
    }

    uses_x_ = math_parser_->GetVariableNames().contains("x");
    if(uses_x_)
        math_parser_->DefineVariable("x", &x_);
}

void ExpressionEvaluator::RegisterVariableValueHandler(const MathParser::VariableFactoryHandler& handler) {
    math_parser_->SetVariableFactory(handler);
}

float ExpressionEvaluator::Evaluate(std::optional<float> x) {
    if(uses_x_) {
        if(!x.has_value())
            throw std::invalid_argument("Expression uses x but no input value was given.");

        x_ = x.value();
    }

    try {
        return math_parser_->Evaluate();
    } catch(const ParserError& e) {
        throw std::runtime_error("Expression evaluation failed: " + e.GetMsg());
    }
}

const std::string& ExpressionEvaluator::GetExpression() const {
    return expression_;
}

const std::unordered_set<std::string>& ExpressionEvaluator::GetVariableNames() const {
    return math_parser_->GetVariableNames();
}

} // namespace eerie_leap::subsys::math_parser
