#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <string>
#include <zephyr/ztest.h>

#include "utilities/string/string_helpers.h"
#include "subsys/math_parser/expression_evaluator.h"

using namespace eerie_leap::utilities::string;
using namespace eerie_leap::subsys::math_parser;

ZTEST_SUITE(expression_evaluator, NULL, NULL, NULL, NULL, NULL);

std::unordered_map<std::string, float> expression_evaluator_variables;

ZTEST(expression_evaluator, test_Evaluate_x_returns_x) {
    ExpressionEvaluator expression_evaluator("x");

    float res = expression_evaluator.Evaluate(8.0);

    zassert_equal(res, 8.0);
}

ZTEST(expression_evaluator, test_Evaluate_braced_x_returns_correct_value) {
    ExpressionEvaluator expression_evaluator("(x + y) * 4");

    expression_evaluator.RegisterVariableValueHandler(
        [&expression_evaluator_variables](const std::string& variable_name) {
            return &expression_evaluator_variables[variable_name];
        });

    expression_evaluator_variables.clear();
    expression_evaluator_variables["y"] = 2.0;

    float res = expression_evaluator.Evaluate(8.0);

    zassert_equal(res, 40.0);
}

ZTEST(expression_evaluator, test_Evaluate_not_braced_x_returns_correct_value) {
    ExpressionEvaluator expression_evaluator("(x + y) * 4");

    expression_evaluator.RegisterVariableValueHandler(
        [&expression_evaluator_variables](const std::string& variable_name) {
            return &expression_evaluator_variables[variable_name];
        });

    expression_evaluator_variables.clear();
    expression_evaluator_variables["y"] = 2.0;

    float res = expression_evaluator.Evaluate(8.0);

    zassert_equal(res, 40.0);
}

// Parser errors surface as std::exception so configuration validation and the processors can catch them.
ZTEST(expression_evaluator, test_invalid_expression_throws_invalid_argument) {
    for(const char* expression : { "", "x +", "2 * (x", "unknown_fn(x)" }) {
        bool thrown = false;

        try {
            ExpressionEvaluator expression_evaluator(expression);
        } catch(const std::invalid_argument&) {
            thrown = true;
        }

        zassert_true(thrown, "Expression \"%s\" was accepted.", expression);
    }
}

ZTEST(expression_evaluator, test_Evaluate_without_x_throws_when_expression_uses_x) {
    ExpressionEvaluator with_x("x * 2");
    ExpressionEvaluator without_x("2 * 3");

    bool thrown = false;
    try {
        with_x.Evaluate();
    } catch(const std::invalid_argument&) {
        thrown = true;
    }

    zassert_true(thrown);
    zassert_equal(without_x.Evaluate(), 6.0F, "An expression without x needs no input");
    zassert_equal(without_x.Evaluate(8.0F), 6.0F, "A given input is ignored without x");
}

ZTEST(expression_evaluator, test_multiple_ExpressionEvaluator_eval_correctly) {
    ExpressionEvaluator expression_evaluator_1("(x + y) * 4");
    ExpressionEvaluator expression_evaluator_2("(x - 8 * var_d) / f");

    expression_evaluator_1.RegisterVariableValueHandler(
        [&expression_evaluator_variables](const std::string& variable_name) {
            return &expression_evaluator_variables[variable_name];
        });

    expression_evaluator_2.RegisterVariableValueHandler(
        [&expression_evaluator_variables](const std::string& variable_name) {
            return &expression_evaluator_variables[variable_name];
        });

    expression_evaluator_variables.clear();
    expression_evaluator_variables["y"] = 2.0;

    float res1 = expression_evaluator_1.Evaluate(8.0);
    zassert_equal(res1, 40.0);

    expression_evaluator_variables["var_d"] = 2.0;
    expression_evaluator_variables["f"] = 2.0;
    float res2 = expression_evaluator_2.Evaluate(80.0);
    zassert_equal(res2, 32.0);
}

ZTEST(expression_evaluator, test_GetExpression_returns_sanitized_expression) {
    ExpressionEvaluator expression_evaluator_1("(x + y) * 4");
    ExpressionEvaluator expression_evaluator_2("(x - 8 * var_d) / f");

    zassert_equal(expression_evaluator_1.GetExpression(), "(x + y) * 4");
    zassert_equal(expression_evaluator_2.GetExpression(), "(x - 8 * var_d) / f");
}

ZTEST(expression_evaluator, test_GetVariableNames_returns_list_of_vars) {
    ExpressionEvaluator expression_evaluator_1("x - 16");
    ExpressionEvaluator expression_evaluator_2("(x + y) * 4");
    ExpressionEvaluator expression_evaluator_3("(x - 8 * var_d) / f");

    auto vars1 = expression_evaluator_1.GetVariableNames();
    zassert_equal(vars1.size(), 1);
    zassert_equal(vars1.count("x"), 1);

    auto vars2 = expression_evaluator_2.GetVariableNames();
    zassert_equal(vars2.size(), 2);
    zassert_equal(vars2.count("x"), 1);
    zassert_equal(vars2.count("y"), 1);

    auto vars3 = expression_evaluator_3.GetVariableNames();
    zassert_equal(vars3.size(), 3);
    zassert_equal(vars3.count("x"), 1);
    zassert_equal(vars3.count("var_d"), 1);
    zassert_equal(vars3.count("f"), 1);
}
