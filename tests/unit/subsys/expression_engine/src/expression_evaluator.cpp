#include <cmath>
#include <string>
#include <vector>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/expression_engine/expression_evaluator.h"

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::expression_engine;

ZTEST_SUITE(expression_evaluator, NULL, NULL, NULL, NULL, NULL);

namespace {

ExpressionEvaluator Make(const char* expression) {
    auto evaluator = ExpressionEvaluator::Create(expression, Mrm::GetDefaultPmr());
    zassert_true(evaluator.has_value(), "Expression \"%s\" was rejected.", expression);

    return std::move(*evaluator);
}

std::vector<std::string> Names(const ExpressionEvaluator& evaluator) {
    std::vector<std::string> names;
    for(const std::string_view name : evaluator.GetVariableNames())
        names.emplace_back(name);

    return names;
}

} // namespace

ZTEST(expression_evaluator, test_Evaluate_x_returns_x) {
    auto evaluator = Make("x");

    zassert_true(evaluator.UsesInput());
    zassert_equal(evaluator.Evaluate(8.0F).value(), 8.0F);
}

ZTEST(expression_evaluator, test_Evaluate_reads_bound_variables) {
    auto evaluator = Make("(x + y) * 4");

    float y = 2.0F;
    zassert_true(evaluator.BindVariable("y", &y));

    zassert_equal(evaluator.Evaluate(8.0F).value(), 40.0F);

    y = 3.0F;
    zassert_equal(evaluator.Evaluate(8.0F).value(), 44.0F, "The address is read on every evaluation");
}

ZTEST(expression_evaluator, test_invalid_expression_is_a_compile_error_with_a_position) {
    for(const char* expression : { "", "x +", "2 * (x", "unknown_fn(x)" }) {
        auto evaluator = ExpressionEvaluator::Create(expression, Mrm::GetDefaultPmr());
        zassert_false(evaluator.has_value(), "Expression \"%s\" was accepted.", expression);

        const auto message = ExpressionEvaluator::Describe(evaluator.error());
        zassert_true(message.starts_with("Invalid expression: "), "%s", message.c_str());
        zassert_true(message.find(" at ") != std::string::npos, "%s", message.c_str());
    }
}

ZTEST(expression_evaluator, test_Evaluate_without_x_fails_when_expression_uses_x) {
    auto with_x = Make("x * 2");
    auto without_x = Make("2 * 3");

    zassert_false(with_x.Evaluate().has_value());
    zassert_false(without_x.UsesInput());
    zassert_equal(without_x.Evaluate().value(), 6.0F, "An expression without x needs no input");
    zassert_equal(without_x.Evaluate(8.0F).value(), 6.0F, "A given input is ignored without x");
}

ZTEST(expression_evaluator, test_Evaluate_fails_while_a_variable_is_unbound) {
    auto evaluator = Make("(x - 8 * var_d) / f");

    float var_d = 2.0F;
    float f = 2.0F;

    zassert_false(evaluator.Evaluate(80.0F).has_value());
    zassert_true(evaluator.BindVariable("var_d", &var_d));
    zassert_false(evaluator.Evaluate(80.0F).has_value());
    zassert_true(evaluator.BindVariable("f", &f));
    zassert_equal(evaluator.Evaluate(80.0F).value(), 32.0F);
}

ZTEST(expression_evaluator, test_BindVariable_rejects_unknown_names_x_and_null) {
    auto evaluator = Make("x + y");
    float value = 1.0F;

    zassert_false(evaluator.BindVariable("z", &value));
    zassert_false(evaluator.BindVariable("x", &value), "x is the input, not a bound variable");
    zassert_false(evaluator.BindVariable("y", nullptr));
    zassert_true(evaluator.BindVariable("y", &value));
}

ZTEST(expression_evaluator, test_NaN_propagates_to_the_caller) {
    auto evaluator = Make("sqrt(x)");

    const auto value = evaluator.Evaluate(-1.0F);
    zassert_true(value.has_value());
    zassert_true(std::isnan(*value));
}

ZTEST(expression_evaluator, test_GetExpression_returns_the_text) {
    zassert_true(Make("(x + y) * 4").GetExpression() == "(x + y) * 4");
    zassert_true(Make("(x - 8 * var_d) / f").GetExpression() == "(x - 8 * var_d) / f");
}

ZTEST(expression_evaluator, test_GetVariableNames_lists_sensor_ids_in_program_order_without_x) {
    zassert_true(Names(Make("x - 16")).empty());

    const auto names_2 = Names(Make("(x + y) * 4"));
    zassert_equal(names_2.size(), 1U);
    zassert_true(names_2[0] == "y");

    const auto names_3 = Names(Make("(x - 8 * var_d) / f + var_d"));
    zassert_equal(names_3.size(), 2U);
    zassert_true(names_3[0] == "var_d");
    zassert_true(names_3[1] == "f");

    zassert_equal(Make("(x - 8 * var_d) / f").GetVariableCount(), 3U, "Including x");
}

ZTEST(expression_evaluator, test_evaluators_are_independent) {
    auto evaluator_1 = Make("(x + y) * 4");
    auto evaluator_2 = Make("(x - 8 * y) / f");

    float y_1 = 2.0F;
    float y_2 = 2.0F;
    float f = 2.0F;
    evaluator_1.BindVariable("y", &y_1);
    evaluator_2.BindVariable("y", &y_2);
    evaluator_2.BindVariable("f", &f);

    zassert_equal(evaluator_1.Evaluate(8.0F).value(), 40.0F);
    zassert_equal(evaluator_2.Evaluate(80.0F).value(), 32.0F);
}
