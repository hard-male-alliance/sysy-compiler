#include "compiler/semantic.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace sysy {
namespace {

/** Semantic walker keeps lexical scopes separate from stable public symbols. /
 * 语义遍历器将词法作用域与稳定的公开符号分离。 */
class Analyzer {
public:
    SemanticResult
    run(const Program& program) {
        scopes_.emplace_back();
        install_builtins();
        for (const auto& item : program.items) {
            if (item.declaration)
                declaration(*item.declaration, true);
            if (item.function)
                function(*item.function);
        }
        for (const auto& symbol : result_.model.symbols) {
            if (symbol.is_function && !symbol.is_builtin && !symbol.function)
                error(
                    symbol.declaration_range,
                    "function '" + symbol.name + "' declared but not defined"
                );
        }
        const auto it = scopes_.front().find("main");
        if (it == scopes_.front().end()
            || !it->second->is_function
            || it->second->type.base != BaseType::Int
            || !it->second->parameters.empty()
            || it->second->is_builtin
            || !it->second->function) {
            error({}, "program requires exactly one int main() definition");
        }
        return std::move(result_);
    }

private:
    SemanticResult result_;
    std::vector<std::unordered_map<std::string, Symbol*>> scopes_;
    const Function* current_function_ = nullptr;
    int loop_depth_ = 0;

    void
    error(SourceRange range, std::string message) {
        result_.errors.push_back({range, std::move(message)});
    }

    /** Keep the primary range on the offending declaration and point to its predecessor. /
     * 主范围标注冲突声明，并在消息中指出前次声明的位置。 */
    static std::string
    previous_at(SourceRange range) {
        return " (previous declaration at "
               + std::to_string(range.begin.line)
               + ":"
               + std::to_string(range.begin.column)
               + ")";
    }

    Symbol*
    add(std::string name, Type type, SourceRange range, bool builtin = false) {
        auto& scope = scopes_.back();
        if (scope.contains(name)) {
            error(range, "duplicate definition of '" + name + "'");
            return nullptr;
        }
        result_.model.symbols.emplace_back();
        auto* symbol = &result_.model.symbols.back();
        symbol->name = std::move(name);
        symbol->type = std::move(type);
        symbol->declaration_range = range;
        symbol->is_global = scopes_.size() == 1;
        symbol->is_builtin = builtin;
        scope.emplace(symbol->name, symbol);
        return symbol;
    }

    Symbol*
    find(std::string_view name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->find(std::string(name)); found != it->end())
                return found->second;
        }
        return nullptr;
    }

    /** Call syntax resolves the global function namespace independently of local objects. /
     * 调用语法独立于局部对象解析全局函数命名空间。 */
    Symbol*
    find_function(std::string_view name) {
        if (auto found = scopes_.front().find(std::string(name));
            found != scopes_.front().end() && found->second->is_function)
            return found->second;
        return nullptr;
    }

    void
    builtin(std::string name, BaseType ret, std::initializer_list<Type> args) {
        auto* symbol = add(std::move(name), {ret, {}}, {}, true);
        symbol->is_function = true;
        symbol->parameters.assign(args);
    }

    void
    install_builtins() {
        const Type i{BaseType::Int, {}}, f{BaseType::Float, {}};
        const Type ia{BaseType::Int, {-1}}, fa{BaseType::Float, {-1}};
        builtin("getint", BaseType::Int, {});
        builtin("getch", BaseType::Int, {});
        builtin("getfloat", BaseType::Float, {});
        builtin("getarray", BaseType::Int, {ia});
        builtin("getfarray", BaseType::Int, {fa});
        builtin("putint", BaseType::Void, {i});
        builtin("putch", BaseType::Void, {i});
        builtin("putfloat", BaseType::Void, {f});
        builtin("putarray", BaseType::Void, {i, ia});
        builtin("putfarray", BaseType::Void, {i, fa});
        builtin("starttime", BaseType::Void, {});
        builtin("stoptime", BaseType::Void, {});
        // `putf` is explicitly optional in the task supplement and intentionally excluded.
    }

    /** Convert a folded value without invoking undefined float-to-int behavior. /
     * 转换折叠值，避免浮点到整型的未定义行为。 */
    static std::optional<ScalarValue>
    convert(ScalarValue value, BaseType to) {
        if (to == BaseType::Float) {
            return std::holds_alternative<float>(value)
                       ? value
                       : ScalarValue{static_cast<float>(std::get<std::int32_t>(value))};
        }
        if (to == BaseType::Int) {
            if (auto* i = std::get_if<std::int32_t>(&value))
                return *i;
            const auto f = std::get<float>(value);
            if (!std::isfinite(f)
                || static_cast<double>(f) < -2147483648.0
                || static_cast<double>(f) >= 2147483648.0)
                return std::nullopt;
            return static_cast<std::int32_t>(f);
        }
        return std::nullopt;
    }

    static bool
    truth(ScalarValue value) {
        return std::holds_alternative<float>(value) ? std::get<float>(value) != 0.0f
                                                    : std::get<std::int32_t>(value) != 0;
    }

    std::optional<std::int64_t>
    integer_literal(const Expr& node) {
        std::string_view spelling = node.text;
        int base = 10;
        if (spelling.size() > 2
            && spelling[0] == '0'
            && (spelling[1] == 'x' || spelling[1] == 'X')) {
            base = 16;
            spelling.remove_prefix(2);
        } else if (spelling.size() > 1 && spelling[0] == '0')
            base = 8;
        std::uint64_t value = 0;
        auto [end, ec] =
            std::from_chars(spelling.data(), spelling.data() + spelling.size(), value, base);
        if (ec != std::errc{}
            || end != spelling.data() + spelling.size()
            || value > 2147483648ULL) {
            error(node.range, "integer literal is outside supported 32-bit range");
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    }

    /** Resolve an expression once and memoize its type and optional constant. /
     * 每个表达式仅解析一次，并缓存类型与可选常量。 */
    TypedExpr
    expression(const Expr& node) {
        if (auto it = result_.model.expressions.find(&node); it != result_.model.expressions.end())
            return it->second;
        TypedExpr out;
        out.type.base = BaseType::Int;
        switch (node.kind) {
        case ExprKind::Integer: {
            auto value = integer_literal(node);
            if (value && *value <= INT32_MAX)
                out.constant = static_cast<std::int32_t>(*value);
            else if (value)
                error(node.range, "integer literal requires unary minus to fit int32");
            break;
        }
        case ExprKind::Float: {
            out.type.base = BaseType::Float;
            errno = 0;
            char* end = nullptr;
            const float value = std::strtof(node.text.c_str(), &end);
            if (end != node.text.c_str() + node.text.size() || !std::isfinite(value))
                error(node.range, "invalid or non-finite float literal");
            else
                out.constant = value;
            break;
        }
        case ExprKind::String:
            error(
                node.range,
                "string literal is only supported by optional putf runtime extension"
            );
            break;
        case ExprKind::Variable:
            out = variable(node);
            break;
        case ExprKind::Call:
            out = call(node);
            break;
        case ExprKind::Unary:
            out = unary(node);
            break;
        case ExprKind::Binary:
            out = binary(node);
            break;
        }
        result_.model.expressions.emplace(&node, out);
        return out;
    }

    TypedExpr
    variable(const Expr& node) {
        TypedExpr out;
        auto* symbol = find(node.text);
        if (!symbol || symbol->is_function) {
            error(node.range, "undefined object '" + node.text + "'");
            return out;
        }
        result_.model.references[&node] = symbol;
        out.type = symbol->type;
        out.lvalue = true;
        for (const auto& child : node.children) {
            auto index = expression(*child);
            if (!index.type.is_scalar() || index.type.base != BaseType::Int)
                error(child->range, "array subscript must have int type");
            if (out.type.dimensions.empty()) {
                error(child->range, "too many array subscripts");
                break;
            }
            out.type.dimensions.erase(out.type.dimensions.begin());
        }
        out.assignable = out.type.is_scalar() && !symbol->is_const;
        if (symbol->is_const && node.children.empty())
            out.constant = symbol->scalar_constant;
        if (symbol->is_const
            && !node.children.empty()
            && out.type.is_scalar()
            && !symbol->initializer.empty()) {
            std::size_t offset = 0;
            bool valid = true;
            for (std::size_t j = 0; j < node.children.size() && j < symbol->type.dimensions.size();
                 ++j) {
                const auto& index = result_.model.expressions.at(node.children[j].get()).constant;
                if (!index || !std::holds_alternative<std::int32_t>(*index)) {
                    valid = false;
                    break;
                }
                const auto v = std::get<std::int32_t>(*index);
                if (v < 0 || v >= symbol->type.dimensions[j]) {
                    valid = false;
                    break;
                }
                std::size_t stride = 1;
                for (std::size_t k = j + 1; k < symbol->type.dimensions.size(); ++k)
                    stride *= static_cast<std::size_t>(symbol->type.dimensions[k]);
                offset += static_cast<std::size_t>(v) * stride;
            }
            if (valid && offset < symbol->initializer.size())
                out.constant = symbol->initializer[offset].constant;
        }
        return out;
    }

    /** Match array arguments structurally; the first formal extent is unknown. /
     * 按结构匹配数组参数；形参首维长度未知。 */
    TypedExpr
    call(const Expr& node) {
        TypedExpr out;
        auto* symbol = find_function(node.text);
        if (!symbol || !symbol->is_function) {
            error(node.range, "undefined function '" + node.text + "'");
            for (auto& arg : node.children)
                expression(*arg);
            return out;
        }
        result_.model.references[&node] = symbol;
        out.type = symbol->type;
        if (node.children.size() != symbol->parameters.size())
            error(node.range, "wrong number of arguments to '" + node.text + "'");
        for (std::size_t j = 0; j < node.children.size(); ++j) {
            auto actual = expression(*node.children[j]);
            if (j >= symbol->parameters.size())
                continue;
            const auto& expected = symbol->parameters[j];
            if (expected.is_scalar()) {
                if (!actual.type.is_scalar() || actual.type.base == BaseType::Void)
                    error(node.children[j]->range, "scalar argument required");
            } else if (
                actual.type.base != expected.base
                || actual.type.dimensions.size() != expected.dimensions.size()
                || !std::equal(
                    actual.type.dimensions.begin() + (actual.type.dimensions.empty() ? 0 : 1),
                    actual.type.dimensions.end(),
                    expected.dimensions.begin() + 1
                )
            ) {
                error(node.children[j]->range, "array argument shape or element type mismatch");
            }
        }
        return out;
    }

    TypedExpr
    unary(const Expr& node) {
        TypedExpr out;
        if (node.children.empty())
            return out;
        if (node.text == "-" && node.children.front()->kind == ExprKind::Integer) {
            auto magnitude = integer_literal(*node.children.front());
            if (magnitude && *magnitude == 2147483648LL) {
                result_.model.expressions.emplace(node.children.front().get(), TypedExpr{});
                out.constant = INT32_MIN;
                return out;
            }
        }
        auto operand = expression(*node.children.front());
        out.type = operand.type;
        if (!operand.type.is_scalar() || operand.type.base == BaseType::Void)
            error(node.range, "unary operator requires scalar operand");
        if (node.text == "!")
            out.type.base = BaseType::Int;
        if (operand.constant) {
            if (node.text == "!")
                out.constant = static_cast<std::int32_t>(!truth(*operand.constant));
            else if (node.text == "+")
                out.constant = operand.constant;
            else if (node.text == "-" && std::holds_alternative<float>(*operand.constant))
                out.constant = -std::get<float>(*operand.constant);
            else if (node.text == "-" && std::get<std::int32_t>(*operand.constant) != INT32_MIN)
                out.constant = -std::get<std::int32_t>(*operand.constant);
        }
        return out;
    }

    /** Fold only defined int32 operations; overflow and zero division remain runtime-undefined. /
     * 只折叠定义良好的 int32 运算；溢出与除零仍属运行时未定义。 */
    TypedExpr
    binary(const Expr& node) {
        TypedExpr out;
        if (node.children.size() != 2)
            return out;
        auto lhs = expression(*node.children[0]);
        auto rhs = expression(*node.children[1]);
        const auto& op = node.text;
        if (!lhs.type.is_scalar()
            || !rhs.type.is_scalar()
            || lhs.type.base == BaseType::Void
            || rhs.type.base == BaseType::Void)
            error(node.range, "binary operator requires scalar operands");
        const bool logical = op == "&&" || op == "||";
        const bool relation =
            op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";
        out.type.base = (logical || relation)
                            ? BaseType::Int
                            : (lhs.type.base == BaseType::Float || rhs.type.base == BaseType::Float
                                   ? BaseType::Float
                                   : BaseType::Int);
        if (op == "%" && out.type.base != BaseType::Int)
            error(node.range, "remainder requires int operands");
        if (logical
            && lhs.constant
            && ((op == "&&" && !truth(*lhs.constant)) || (op == "||" && truth(*lhs.constant)))) {
            out.constant = static_cast<std::int32_t>(op == "||");
            return out;
        }
        if (!lhs.constant || !rhs.constant)
            return out;
        const bool float_op = lhs.type.base == BaseType::Float || rhs.type.base == BaseType::Float;
        if (logical)
            out.constant = static_cast<std::int32_t>(
                op == "&&" ? truth(*lhs.constant) && truth(*rhs.constant)
                           : truth(*lhs.constant) || truth(*rhs.constant)
            );
        else if (float_op) {
            const float a = std::get<float>(*convert(*lhs.constant, BaseType::Float));
            const float b = std::get<float>(*convert(*rhs.constant, BaseType::Float));
            if (relation)
                out.constant = static_cast<std::int32_t>(
                    op == "<"    ? a < b
                    : op == "<=" ? a <= b
                    : op == ">"  ? a > b
                    : op == ">=" ? a >= b
                    : op == "==" ? a == b
                                 : a != b
                );
            else if (op == "+")
                out.constant = a + b;
            else if (op == "-")
                out.constant = a - b;
            else if (op == "*")
                out.constant = a * b;
            else if (op == "/" && b != 0)
                out.constant = a / b;
        } else {
            const std::int64_t a = std::get<std::int32_t>(*lhs.constant),
                               b = std::get<std::int32_t>(*rhs.constant);
            if (relation)
                out.constant = static_cast<std::int32_t>(
                    op == "<"    ? a < b
                    : op == "<=" ? a <= b
                    : op == ">"  ? a > b
                    : op == ">=" ? a >= b
                    : op == "==" ? a == b
                                 : a != b
                );
            else if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
                if ((op == "/" || op == "%") && b == 0)
                    return out;
                const auto value = op == "+"   ? a + b
                                   : op == "-" ? a - b
                                   : op == "*" ? a * b
                                   : op == "/" ? a / b
                                               : a % b;
                if (value >= INT32_MIN && value <= INT32_MAX)
                    out.constant = static_cast<std::int32_t>(value);
            }
        }
        return out;
    }

    std::optional<std::int64_t>
    dimension(const Expr& expr) {
        auto value = expression(expr);
        if (!value.type.is_scalar()
            || value.type.base != BaseType::Int
            || !value.constant
            || !std::holds_alternative<std::int32_t>(*value.constant)
            || std::get<std::int32_t>(*value.constant) < 0) {
            error(expr.range, "array dimension must be a nonnegative compile-time int");
            return std::nullopt;
        }
        const auto size = std::get<std::int32_t>(*value.constant);
        return size;
    }

    /** Bound every nonzero shape factor, including zero-extent suffixes used in pointer arithmetic.
     * / 限制所有非零维因子的乘积，包括用于地址运算的零长度数组后缀。 */
    static std::optional<std::size_t>
    extent(const Type& type, bool parameter = false) {
        std::size_t count = 1;
        std::size_t nonzero_product = 1;
        for (std::size_t i = 0; i < type.dimensions.size(); ++i) {
            const auto dim = type.dimensions[i];
            if (parameter && i == 0 && dim == -1)
                continue;
            if (dim < 0)
                return std::nullopt;
            if (dim == 0) {
                count = 0;
                continue;
            }
            if (static_cast<std::uint64_t>(dim) > 10000000 / nonzero_product)
                return std::nullopt;
            nonzero_product *= static_cast<std::size_t>(dim);
            if (count != 0)
                count *= static_cast<std::size_t>(dim);
        }
        return count;
    }

    void
    scalar_init(
        const Initializer& init,
        BaseType target,
        InitSlot& slot,
        bool constant_required,
        bool array_element = false
    ) {
        if (init.is_aggregate()) {
            if (init.elements.size() > 1)
                error(init.range, "too many elements for scalar initializer");
            if (!init.elements.empty())
                scalar_init(init.elements.front(), target, slot, constant_required, array_element);
            return;
        }
        auto value = expression(*init.expr);
        if (!value.type.is_scalar() || value.type.base == BaseType::Void) {
            error(init.range, "scalar initializer requires scalar expression");
            return;
        }
        if (constant_required && !value.constant)
            error(init.range, "initializer must be a compile-time constant");
        if (array_element && target == BaseType::Int && value.type.base == BaseType::Float)
            error(init.range, "float expression cannot initialize an int array element");
        slot.expression = init.expr.get();
        slot.constant.reset();
        if (value.constant) {
            slot.constant = convert(*value.constant, target);
            if (!slot.constant)
                error(init.range, "constant initializer is outside target type range");
        }
    }

    /** Align nested braces at subarray boundaries while preserving expression order. /
     * 在子数组边界对齐嵌套花括号，同时保持表达式顺序。 */
    void
    flatten(
        const Initializer& init,
        const Type& type,
        std::vector<InitSlot>& slots,
        std::size_t start,
        std::size_t count,
        std::size_t rank,
        bool constant_required
    ) {
        if (count == 0) {
            if (!init.is_aggregate() || !init.elements.empty())
                error(init.range, "zero-extent array accepts only empty braces");
            return;
        }
        if (rank == type.dimensions.size()) {
            scalar_init(init, type.base, slots[start], constant_required, true);
            return;
        }
        if (!init.is_aggregate()) {
            error(init.range, "array initializer requires braces");
            return;
        }
        std::size_t cursor = 0;
        const auto child = count / static_cast<std::size_t>(type.dimensions[rank]);
        for (const auto& element : init.elements) {
            if (element.is_aggregate()) {
                if (cursor % child)
                    cursor += child - cursor % child;
                if (cursor >= count) {
                    error(element.range, "too many array initializer elements");
                    continue;
                }
                flatten(element, type, slots, start + cursor, child, rank + 1, constant_required);
                cursor += child;
            } else {
                if (cursor >= count) {
                    error(element.range, "too many array initializer elements");
                    continue;
                }
                scalar_init(element, type.base, slots[start + cursor], constant_required, true);
                ++cursor;
            }
        }
    }

    /** Publish a symbol before its initializer so self-reference is diagnosed as nonconstant. /
     * 在初始化前发布符号，使自引用被判为非常量。 */
    void
    declaration(const Declaration& decl, bool global) {
        for (const auto& def : decl.definitions) {
            Type type{decl.type, {}};
            for (const auto& expr : def.dimensions)
                type.dimensions.push_back(dimension(*expr).value_or(1));
            auto* symbol = add(def.name, type, def.range);
            if (!symbol)
                continue;
            symbol->is_const = decl.is_const;
            symbol->is_global = global;
            symbol->definition = &def;
            result_.model.definitions[&def] = symbol;
            if (decl.is_const && !def.initializer)
                error(def.range, "const object requires an initializer");
            auto count = extent(type);
            if (!count || *count > 10000000) {
                error(def.range, "array has invalid or excessive element count");
                continue;
            }
            if (def.initializer || global) {
                symbol->initializer.resize(*count);
                for (auto& slot : symbol->initializer)
                    slot.constant = type.base == BaseType::Float ? ScalarValue{0.0f}
                                                                 : ScalarValue{std::int32_t{0}};
            }
            if (def.initializer) {
                const bool require_constant = global || decl.is_const;
                if (type.is_scalar())
                    scalar_init(
                        *def.initializer,
                        type.base,
                        symbol->initializer.front(),
                        require_constant
                    );
                else
                    flatten(
                        *def.initializer,
                        type,
                        symbol->initializer,
                        0,
                        *count,
                        0,
                        require_constant
                    );
            }
            if (decl.is_const && type.is_scalar() && !symbol->initializer.empty())
                symbol->scalar_constant = symbol->initializer.front().constant;
        }
    }

    void
    statement(const Statement& stmt, bool function_body = false) {
        switch (stmt.kind) {
        case StmtKind::Block:
            if (!function_body)
                scopes_.emplace_back();
            for (const auto& item : stmt.items) {
                if (item.declaration)
                    declaration(*item.declaration, false);
                if (item.statement)
                    statement(*item.statement);
            }
            if (!function_body)
                scopes_.pop_back();
            break;
        case StmtKind::Assignment:
            if (stmt.target) {
                auto target = expression(*stmt.target);
                if (!target.assignable)
                    error(stmt.target->range, "assignment target must be a mutable scalar object");
                if (stmt.expr) {
                    auto value = expression(*stmt.expr);
                    if (!value.type.is_scalar() || value.type.base == BaseType::Void)
                        error(stmt.expr->range, "assignment value must be scalar");
                }
            }
            break;
        case StmtKind::Expression:
            if (stmt.expr)
                expression(*stmt.expr);
            break;
        case StmtKind::If:
        case StmtKind::While:
            if (stmt.expr) {
                auto cond = expression(*stmt.expr);
                if (!cond.type.is_scalar() || cond.type.base == BaseType::Void)
                    error(stmt.expr->range, "condition must be a numeric scalar");
            }
            if (stmt.kind == StmtKind::While)
                ++loop_depth_;
            if (stmt.first)
                statement(*stmt.first);
            if (stmt.second)
                statement(*stmt.second);
            if (stmt.kind == StmtKind::While)
                --loop_depth_;
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            if (loop_depth_ == 0)
                error(stmt.range, "break or continue must be inside a while loop");
            break;
        case StmtKind::Return:
            if (stmt.expr) {
                auto value = expression(*stmt.expr);
                if (current_function_->return_type == BaseType::Void)
                    error(stmt.range, "void function cannot return a value");
                else if (!value.type.is_scalar() || value.type.base == BaseType::Void)
                    error(stmt.expr->range, "return value must be scalar");
            } else if (current_function_->return_type != BaseType::Void)
                error(stmt.range, "non-void function must return a value");
            break;
        }
    }

    void
    function(const Function& func) {
        const SourceRange header{
            func.range.begin,
            func.body ? func.body->range.begin : func.range.end
        };
        // Resolve bounds in parameter scope, so earlier names shadow global constants.
        // 在形参作用域解析维度，确保先前的形参名能遮蔽全局常量。
        scopes_.emplace_back();
        std::vector<Type> signature;
        std::vector<Symbol*> parameter_objects;
        for (const auto& param : func.parameters) {
            Type type{param.type, {}};
            if (param.is_array) {
                type.dimensions.push_back(-1);
                for (const auto& expr : param.dimensions)
                    type.dimensions.push_back(dimension(*expr).value_or(1));
                if (!extent(type, true))
                    error(param.range, "array parameter dimensions exceed supported stride range");
            }
            signature.push_back(type);
            if (param.name.empty()) {
                if (func.body)
                    error(param.range, "function definition requires a name for every parameter");
                continue;
            }
            auto* object = add(param.name, type, param.range);
            if (object && func.body) {
                object->parameter = &param;
                result_.model.parameters[&param] = object;
                parameter_objects.push_back(object);
            }
        }
        scopes_.pop_back();

        auto& global = scopes_.front();
        Symbol* symbol = nullptr;
        if (const auto found = global.find(func.name); found != global.end()) {
            symbol = found->second;
            if (!symbol->is_function
                || symbol->type.base != func.return_type
                || symbol->parameters.size() != signature.size()
                || !std::equal(
                    signature.begin(),
                    signature.end(),
                    symbol->parameters.begin(),
                    [](const Type& a, const Type& b) {
                        return a.base == b.base && a.dimensions == b.dimensions;
                    }
                )) {
                error(
                    header,
                    "conflicting declaration of function '"
                        + func.name
                        + "'"
                        + (symbol->is_builtin ? "" : previous_at(symbol->declaration_range))
                );
                return;
            }
            if (func.body && (symbol->function || symbol->is_builtin)) {
                error(
                    header,
                    "duplicate definition of function '"
                        + func.name
                        + "'"
                        + (symbol->is_builtin ? ""
                                              : previous_at(
                                                    symbol->function ? symbol->function->range
                                                                     : symbol->declaration_range
                                                ))
                );
                return;
            }
        } else {
            symbol = add(func.name, {func.return_type, {}}, header);
            symbol->is_function = true;
            symbol->parameters = std::move(signature);
        }
        result_.model.functions[&func] = symbol;
        if (!func.body)
            return;
        symbol->function = &func;
        current_function_ = &func;
        scopes_.emplace_back();
        for (auto* object : parameter_objects)
            scopes_.back().emplace(object->name, object);
        statement(*func.body, true);
        scopes_.pop_back();
        current_function_ = nullptr;
    }
};
} // namespace

SemanticResult
analyze(const Program& program) {
    return Analyzer{}.run(program);
}
} // namespace sysy
