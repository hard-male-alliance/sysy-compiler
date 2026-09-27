#include "compiler/lower.hpp"

#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace sysy {
namespace {

/** Map a checked SysY scalar or array-address type to IR. / 将已检查的 SysY
 * 标量或数组地址类型映射到 IR。 */
IrType
ir_type(const Type& type) {
    if (!type.dimensions.empty())
        return IrType::Ptr;
    if (type.base == BaseType::Float)
        return IrType::F32;
    if (type.base == BaseType::Void)
        return IrType::Void;
    return IrType::I32;
}

/** Preserve exact binary32 bits for globals and constants. / 保留全局值与常量的精确 binary32 位型。
 */
std::uint32_t
bits(const ScalarValue& value) {
    if (auto* f = std::get_if<float>(&value))
        return std::bit_cast<std::uint32_t>(*f);
    return std::bit_cast<std::uint32_t>(std::get<std::int32_t>(value));
}

/** Compute byte extent from semantically checked dimensions. / 由已通过语义检查的维度计算字节大小。
 */
std::int64_t
byte_size(const Type& type) {
    std::int64_t size = 4;
    for (auto dim : type.dimensions)
        size *= dim;
    // 中文：零长对象仍需一个可表示的地址；逻辑元素数保持为零。
    // English: A zero-extent object still needs an addressable backing word; logical size remains
    // zero.
    return size == 0 ? 4 : size;
}

/** One function's mutable lowering state; all IDs remain local to its FunctionIR. /
 * 单个函数的降低状态；所有编号仅在其 FunctionIR 内有效。 */
class Builder {
public:
    Builder(FunctionIR& function, const SemanticModel& model, LowerResult& result)
        : function_(function),
          model_(model),
          result_(result) {
        current_ = function_.new_block();
        function_.entry = current_;
    }

    void
    lower_function(const Function& source) {
        for (std::size_t i = 0; i < source.parameters.size(); ++i) {
            const auto& parameter = source.parameters[i];
            const Symbol* symbol = model_.parameters.at(&parameter);
            const IrType type = ir_type(symbol->type);
            function_.parameters.push_back(type);
            const auto incoming =
                emit(IrOp::Param, type, {}, static_cast<std::int64_t>(i), {}, parameter.range);
            if (type == IrType::Ptr) {
                addresses_[symbol] = incoming;
            } else {
                const auto address = stack_alloc(symbol->type, parameter.range);
                addresses_[symbol] = address;
                emit(IrOp::Store, IrType::Void, {address, incoming}, 0, {}, parameter.range);
            }
        }
        statement(*source.body);
        if (active()) {
            if (function_.return_type == IrType::Void) {
                function_.blocks[current_].terminator = {TermKind::Return};
            } else {
                // 中文：源语言允许值函数末尾落空，返回值未定义；任意值均可实现该未定义情形。
                // English: A fall-through value return is undefined by SysY; choose a stable
                // arbitrary result.
                const auto zero = constant(function_.return_type, 0, source.range);
                function_.blocks[current_].terminator = {TermKind::Return, zero};
            }
        }
        // 中文：不可达续块仍需结构完整，供验证器与后端统一处理。
        // English: Even unreachable continuation blocks need complete terminators.
        for (auto& block : function_.blocks) {
            if (block.terminator.kind == TermKind::None)
                block.terminator = {TermKind::Jump, no_value, function_.entry};
        }
    }

private:
    FunctionIR& function_;
    const SemanticModel& model_;
    LowerResult& result_;
    BlockId current_ = no_block;
    std::unordered_map<const Symbol*, ValueId> addresses_;
    std::vector<std::pair<BlockId, BlockId>> loops_;

    [[nodiscard]] bool
    active() const {
        return current_ != no_block && function_.blocks[current_].terminator.kind == TermKind::None;
    }

    ValueId
    emit(
        IrOp op,
        IrType type,
        std::vector<ValueId> args = {},
        std::int64_t imm = 0,
        std::string symbol = {},
        SourceRange range = {}
    ) {
        Instruction instruction;
        instruction.op = op;
        instruction.type = type;
        instruction.dst = type == IrType::Void ? no_value : function_.new_value(type);
        instruction.args = std::move(args);
        instruction.imm = imm;
        instruction.symbol = std::move(symbol);
        instruction.range = range;
        const ValueId dst = instruction.dst;
        function_.blocks[current_].instructions.push_back(std::move(instruction));
        return dst;
    }

    ValueId
    constant(IrType type, std::uint32_t word, SourceRange range) {
        return emit(
            type == IrType::F32 ? IrOp::ConstF32 : IrOp::ConstI32,
            type,
            {},
            static_cast<std::int64_t>(word),
            {},
            range
        );
    }

    ValueId
    stack_alloc(const Type& type, SourceRange range) {
        const auto save = current_;
        current_ = function_.entry;
        const auto address = emit(IrOp::Alloca, IrType::Ptr, {}, byte_size(type), {}, range);
        function_.blocks[current_].instructions.back().aux = 4;
        current_ = save;
        return address;
    }

    ValueId
    cast(ValueId value, IrType target, SourceRange range) {
        const auto source = function_.value_types[value];
        if (source == target)
            return value;
        if (source == IrType::I32 && target == IrType::F32)
            return emit(IrOp::IToF, target, {value}, 0, {}, range);
        if (source == IrType::F32 && target == IrType::I32)
            return emit(IrOp::FToI, target, {value}, 0, {}, range);
        result_.errors.push_back({range, "unsupported implicit IR conversion"});
        return value;
    }

    ValueId
    address(const Expr& expr) {
        const Symbol* symbol = model_.references.at(&expr);
        ValueId base = no_value;
        if (symbol->is_global)
            base = emit(IrOp::GlobalAddr, IrType::Ptr, {}, 0, symbol->name, expr.range);
        else
            base = addresses_.at(symbol);
        for (std::size_t i = 0; i < expr.children.size(); ++i) {
            const auto index =
                cast(expression(*expr.children[i]), IrType::I32, expr.children[i]->range);
            std::int64_t stride = 4;
            for (std::size_t j = i + 1; j < symbol->type.dimensions.size(); ++j)
                stride *= symbol->type.dimensions[j];
            const auto scale =
                constant(IrType::I32, static_cast<std::uint32_t>(stride), expr.range);
            const auto offset = emit(IrOp::Mul, IrType::I32, {index, scale}, 0, {}, expr.range);
            base = emit(IrOp::PtrAdd, IrType::Ptr, {base, offset}, 0, {}, expr.range);
        }
        return base;
    }

    ValueId
    expression(const Expr& expr) {
        const auto& typed = model_.expressions.at(&expr);
        switch (expr.kind) {
        case ExprKind::Integer:
        case ExprKind::Float:
            return constant(ir_type(typed.type), bits(*typed.constant), expr.range);
        case ExprKind::Variable: {
            const auto ptr = address(expr);
            if (!typed.type.is_scalar())
                return ptr;
            return emit(IrOp::Load, ir_type(typed.type), {ptr}, 0, {}, expr.range);
        }
        case ExprKind::Call: {
            const Symbol* callee = model_.references.at(&expr);
            std::vector<ValueId> args;
            std::string name = callee->name;
            if (name == "starttime" || name == "stoptime") {
                name = name == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
                args.push_back(constant(
                    IrType::I32,
                    static_cast<std::uint32_t>(expr.range.begin.line),
                    expr.range
                ));
            } else {
                for (std::size_t i = 0; i < expr.children.size(); ++i) {
                    auto value = expression(*expr.children[i]);
                    value = cast(value, ir_type(callee->parameters[i]), expr.children[i]->range);
                    args.push_back(value);
                }
            }
            return emit(IrOp::Call, ir_type(callee->type), std::move(args), 0, name, expr.range);
        }
        case ExprKind::Unary: {
            if (expr.text == "!")
                return logical_value(expr);
            // 中文：语义分析专门接受 -2147483648，但其正数字面量本身没有 int32 值。
            // English: Semantics accepts -2147483648 specially; its positive child has no i32
            // value.
            if (expr.text == "-"
                && expr.children[0]->kind == ExprKind::Integer
                && typed.constant
                && std::get_if<std::int32_t>(&*typed.constant)
                && std::get<std::int32_t>(*typed.constant)
                       == std::numeric_limits<std::int32_t>::min())
                return constant(IrType::I32, 0x80000000U, expr.range);
            auto operand = expression(*expr.children[0]);
            const auto type = ir_type(typed.type);
            operand = cast(operand, type, expr.range);
            if (expr.text == "+")
                return operand;
            const auto zero = constant(type, 0, expr.range);
            return emit(IrOp::Sub, type, {zero, operand}, 0, {}, expr.range);
        }
        case ExprKind::Binary: {
            if (expr.text == "&&" || expr.text == "||")
                return logical_value(expr);
            auto lhs = expression(*expr.children[0]);
            auto rhs = expression(*expr.children[1]);
            const bool compare = expr.text == "=="
                                 || expr.text == "!="
                                 || expr.text == "<"
                                 || expr.text == "<="
                                 || expr.text == ">"
                                 || expr.text == ">=";
            const auto operation_type = compare ? ((function_.value_types[lhs] == IrType::F32
                                                    || function_.value_types[rhs] == IrType::F32)
                                                       ? IrType::F32
                                                       : IrType::I32)
                                                : ir_type(typed.type);
            lhs = cast(lhs, operation_type, expr.range);
            rhs = cast(rhs, operation_type, expr.range);
            if (compare) {
                const CmpPred pred = expr.text == "=="   ? CmpPred::Eq
                                     : expr.text == "!=" ? CmpPred::Ne
                                     : expr.text == "<"  ? CmpPred::Lt
                                     : expr.text == "<=" ? CmpPred::Le
                                     : expr.text == ">"  ? CmpPred::Gt
                                                         : CmpPred::Ge;
                return emit(
                    IrOp::Cmp,
                    IrType::I32,
                    {lhs, rhs},
                    static_cast<std::int64_t>(pred),
                    {},
                    expr.range
                );
            }
            const IrOp op = expr.text == "+"   ? IrOp::Add
                            : expr.text == "-" ? IrOp::Sub
                            : expr.text == "*" ? IrOp::Mul
                            : expr.text == "/" ? IrOp::Div
                                               : IrOp::Rem;
            return emit(op, operation_type, {lhs, rhs}, 0, {}, expr.range);
        }
        case ExprKind::String:
            result_.errors.push_back(
                {expr.range, "string literals are not supported by the SysY runtime contract"}
            );
            return constant(IrType::I32, 0, expr.range);
        }
        return no_value;
    }

    void
    condition(const Expr& expr, BlockId yes, BlockId no) {
        if (expr.kind == ExprKind::Unary && expr.text == "!") {
            condition(*expr.children[0], no, yes);
            return;
        }
        if (expr.kind == ExprKind::Binary && expr.text == "&&") {
            const auto rhs = function_.new_block();
            condition(*expr.children[0], rhs, no);
            current_ = rhs;
            condition(*expr.children[1], yes, no);
            return;
        }
        if (expr.kind == ExprKind::Binary && expr.text == "||") {
            const auto rhs = function_.new_block();
            condition(*expr.children[0], yes, rhs);
            current_ = rhs;
            condition(*expr.children[1], yes, no);
            return;
        }
        auto value = expression(expr);
        const auto type = function_.value_types[value];
        const auto zero = constant(type, 0, expr.range);
        value = emit(
            IrOp::Cmp,
            IrType::I32,
            {value, zero},
            static_cast<std::int64_t>(CmpPred::Ne),
            {},
            expr.range
        );
        function_.blocks[current_].terminator = {TermKind::Branch, value, yes, no};
    }

    ValueId
    logical_value(const Expr& expr) {
        const auto yes = function_.new_block();
        const auto no = function_.new_block();
        const auto merge = function_.new_block();
        condition(expr, yes, no);
        current_ = yes;
        const auto one = constant(IrType::I32, 1, expr.range);
        function_.blocks[yes].terminator = {TermKind::Jump, no_value, merge};
        current_ = no;
        const auto zero = constant(IrType::I32, 0, expr.range);
        function_.blocks[no].terminator = {TermKind::Jump, no_value, merge};
        current_ = merge;
        Instruction phi;
        phi.op = IrOp::Phi;
        phi.type = IrType::I32;
        phi.dst = function_.new_value(IrType::I32);
        phi.incoming = {{yes, one}, {no, zero}};
        phi.range = expr.range;
        function_.blocks[merge].instructions.push_back(std::move(phi));
        return function_.blocks[merge].instructions.back().dst;
    }

    void
    declaration(const Declaration& decl) {
        for (const auto& def : decl.definitions) {
            const Symbol* symbol = model_.definitions.at(&def);
            const auto ptr = stack_alloc(symbol->type, def.range);
            addresses_[symbol] = ptr;
            for (std::size_t i = 0; i < symbol->initializer.size(); ++i) {
                const auto& slot = symbol->initializer[i];
                const auto offset =
                    constant(IrType::I32, static_cast<std::uint32_t>(i * 4), def.range);
                const auto cell = emit(IrOp::PtrAdd, IrType::Ptr, {ptr, offset}, 0, {}, def.range);
                ValueId value = no_value;
                if (slot.expression)
                    value = expression(*slot.expression);
                else
                    value = constant(ir_type(Type{symbol->type.base, {}}), 0, def.range);
                value = cast(value, ir_type(Type{symbol->type.base, {}}), def.range);
                emit(IrOp::Store, IrType::Void, {cell, value}, 0, {}, def.range);
            }
        }
    }

    void
    statement(const Statement& stmt) {
        if (!active())
            return;
        switch (stmt.kind) {
        case StmtKind::Block:
            for (const auto& item : stmt.items) {
                if (!active())
                    break;
                if (item.declaration)
                    declaration(*item.declaration);
                else if (item.statement)
                    statement(*item.statement);
            }
            break;
        case StmtKind::Assignment: {
            const auto ptr = address(*stmt.target);
            auto value = expression(*stmt.expr);
            value = cast(value, ir_type(model_.expressions.at(stmt.target.get()).type), stmt.range);
            emit(IrOp::Store, IrType::Void, {ptr, value}, 0, {}, stmt.range);
            break;
        }
        case StmtKind::Expression:
            if (stmt.expr)
                (void)expression(*stmt.expr);
            break;
        case StmtKind::Return: {
            ValueId value = no_value;
            if (stmt.expr)
                value = cast(expression(*stmt.expr), function_.return_type, stmt.range);
            function_.blocks[current_].terminator = {TermKind::Return, value};
            break;
        }
        case StmtKind::If: {
            const auto yes = function_.new_block();
            const auto no = function_.new_block();
            const auto merge = function_.new_block();
            condition(*stmt.expr, yes, no);
            current_ = yes;
            statement(*stmt.first);
            if (active())
                function_.blocks[current_].terminator = {TermKind::Jump, no_value, merge};
            current_ = no;
            if (stmt.second)
                statement(*stmt.second);
            if (active())
                function_.blocks[current_].terminator = {TermKind::Jump, no_value, merge};
            current_ = merge;
            break;
        }
        case StmtKind::While: {
            const auto head = function_.new_block();
            const auto body = function_.new_block();
            const auto exit = function_.new_block();
            function_.blocks[current_].terminator = {TermKind::Jump, no_value, head};
            current_ = head;
            condition(*stmt.expr, body, exit);
            loops_.push_back({head, exit});
            current_ = body;
            statement(*stmt.first);
            if (active())
                function_.blocks[current_].terminator = {TermKind::Jump, no_value, head};
            loops_.pop_back();
            current_ = exit;
            break;
        }
        case StmtKind::Break:
            function_.blocks[current_]
                .terminator = {TermKind::Jump, no_value, loops_.back().second};
            break;
        case StmtKind::Continue:
            function_.blocks[current_].terminator = {TermKind::Jump, no_value, loops_.back().first};
            break;
        }
    }
};

} // namespace

LowerResult
lower(const Program& program, const SemanticModel& model) {
    LowerResult result;
    for (const auto& item : program.items) {
        if (!item.declaration)
            continue;
        for (const auto& def : item.declaration->definitions) {
            const Symbol* symbol = model.definitions.at(&def);
            GlobalIR global;
            global.name = symbol->name;
            global.element_type = ir_type(Type{symbol->type.base, {}});
            global.is_const = symbol->is_const;
            const std::size_t count = static_cast<std::size_t>(byte_size(symbol->type) / 4);
            global.words.assign(count, 0);
            for (std::size_t i = 0; i < symbol->initializer.size() && i < count; ++i) {
                if (symbol->initializer[i].constant)
                    global.words[i] = bits(*symbol->initializer[i].constant);
            }
            result.module.globals.push_back(std::move(global));
        }
    }
    for (const auto& item : program.items) {
        if (!item.function || !item.function->body)
            continue;
        const auto& source = *item.function;
        FunctionIR function;
        function.name = source.name;
        function.return_type = ir_type(Type{source.return_type, {}});
        Builder builder(function, model, result);
        builder.lower_function(source);
        result.module.functions.push_back(std::move(function));
    }
    return result;
}

} // namespace sysy
