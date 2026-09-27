#pragma once

#include "compiler/ast.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <variant>

namespace sysy {

/** Resolved type; -1 denotes an omitted leading array-parameter bound. / 已解析类型；-1
 * 表示数组形参省略的首维。 */
struct Type {
    BaseType base = BaseType::Int;
    std::vector<std::int64_t> dimensions;

    [[nodiscard]] bool
    is_scalar() const {
        return dimensions.empty();
    }
};

/** Compile-time scalar after SysY binary32/int32 conversion. / 经 SysY binary32/int32
 * 转换后的编译期标量。 */
using ScalarValue = std::variant<std::int32_t, float>;

/** Resolved expression semantics; arrays denote addressable subobjects, never scalar values. /
 * 已解析表达式语义；数组表示可寻址子对象，绝非标量值。 */
struct TypedExpr {
    Type type;
    std::optional<ScalarValue> constant;
    bool lvalue = false;
    bool assignable = false;
};

/** A flattened row-major initializer slot. A null expression is an implicit zero. /
 * 按行优先摊平的初值槽；空表达式表示隐式零。 */
struct InitSlot {
    const Expr* expression = nullptr;
    std::optional<ScalarValue> constant;
};

/** Stable resolved declaration or built-in function identity. / 稳定的已解析声明或内建函数身份。 */
struct Symbol {
    std::string name;
    Type type;
    bool is_function = false;
    bool is_builtin = false;
    bool is_const = false;
    bool is_global = false;
    std::vector<Type> parameters;
    std::optional<ScalarValue> scalar_constant;
    std::vector<InitSlot> initializer;
    const Definition* definition = nullptr;
    const Parameter* parameter = nullptr;
    const Function* function = nullptr;
};

/** Typed side table; Symbol addresses remain stable for this model's lifetime. / 类型化侧表；Symbol
 * 地址在模型生命周期内稳定。 */
struct SemanticModel {
    SemanticModel() = default;
    SemanticModel(const SemanticModel&) = delete;
    SemanticModel& operator=(const SemanticModel&) = delete;
    SemanticModel(SemanticModel&&) noexcept = default;
    SemanticModel& operator=(SemanticModel&&) noexcept = default;

    std::deque<Symbol> symbols;
    std::unordered_map<const Expr*, TypedExpr> expressions;
    std::unordered_map<const Expr*, const Symbol*> references;
    std::unordered_map<const Definition*, const Symbol*> definitions;
    std::unordered_map<const Parameter*, const Symbol*> parameters;
    std::unordered_map<const Function*, const Symbol*> functions;
};

/** Semantic analysis result. Inspect errors before using model for code generation. /
 * 语义分析结果；生成代码前须检查 errors。 */
struct SemanticResult {
    SemanticModel model;
    std::vector<FrontendError> errors;

    [[nodiscard]] bool
    ok() const {
        return errors.empty();
    }
};

/** Analyze SysY 2022 syntax and prepare typed, flattened initialization metadata. / 分析 SysY 2022
 * 语法并准备类型化、摊平的初始化元数据。 */
[[nodiscard]] SemanticResult analyze(const Program& program);

} // namespace sysy
