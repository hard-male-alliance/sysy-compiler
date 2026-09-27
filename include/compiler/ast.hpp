#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sysy {

/** Source coordinates are one-based; offsets are zero-based byte positions. /
 * 源坐标行列从一开始，偏移是从零开始的字节位置。 */
struct SourcePosition {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

/** Half-open source interval [begin,end). / 半开源代码区间 [begin,end)。 */
struct SourceRange {
    SourcePosition begin;
    SourcePosition end;
};

/** Front-end error suitable for renderer diagnostics. / 供诊断呈现器使用的前端错误。 */
struct FrontendError {
    SourceRange range;
    std::string message;
};

/** SysY scalar base types. / SysY 标量基础类型。 */
enum class BaseType { Int, Float, Void };

/** Expression shape; children are ordered operands, subscripts, or call arguments. /
 * 表达式形态；children 依次为运算项、下标或调用实参。 */
enum class ExprKind { Integer, Float, String, Variable, Call, Unary, Binary };

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

/** Expression syntax tree; text preserves literal spelling, identifier, or operator. /
 * 表达式语法树；text 保留字面值、标识符或运算符的原始拼写。 */
struct Expr {
    ExprKind kind = ExprKind::Integer;
    SourceRange range;
    std::string text;
    std::vector<ExprPtr> children;
};

/** Recursive initializer; scalar holds expr, aggregate holds elements. / 递归初值；标量保存
 * expr，聚合初值保存 elements。 */
struct Initializer {
    SourceRange range;
    ExprPtr expr;
    std::vector<Initializer> elements;

    [[nodiscard]] bool
    is_aggregate() const {
        return !expr;
    }
};

/** One declared object, with dimensions in source order. / 单个声明对象，维度按源码顺序排列。 */
struct Definition {
    SourceRange range;
    std::string name;
    std::vector<ExprPtr> dimensions;
    std::optional<Initializer> initializer;
};

/** A declaration owns all comma-separated definitions. / 一条声明拥有其逗号分隔的全部定义。 */
struct Declaration {
    SourceRange range;
    bool is_const = false;
    BaseType type = BaseType::Int;
    std::vector<Definition> definitions;
};

/** Array parameter has an omitted first dimension; dimensions holds only subsequent sizes. /
 * 数组形参首维省略，dimensions 只保存其余维度。 */
struct Parameter {
    SourceRange range;
    BaseType type = BaseType::Int;
    std::string name;
    bool is_array = false;
    std::vector<ExprPtr> dimensions;
};

enum class StmtKind { Block, Assignment, Expression, If, While, Break, Continue, Return };

struct Statement;
using StmtPtr = std::shared_ptr<Statement>;

/** Block item stores exactly one declaration or statement. / 语句块项只保存声明或语句之一。 */
struct BlockItem {
    SourceRange range;
    std::optional<Declaration> declaration;
    StmtPtr statement;
};

/** Statement fields: expr is condition/value, target assignment LVal, first/second are branches or
 * loop body. / 语句字段：expr 为条件或值，target 为赋值左值，first/second 为分支或循环体。 */
struct Statement {
    StmtKind kind = StmtKind::Block;
    SourceRange range;
    ExprPtr expr;
    ExprPtr target;
    StmtPtr first;
    StmtPtr second;
    std::vector<BlockItem> items;
};

/** Function definition owns its body, which is a Block statement. / 函数定义拥有 Block
 * 语句形式的函数体。 */
struct Function {
    SourceRange range;
    BaseType return_type = BaseType::Void;
    std::string name;
    std::vector<Parameter> parameters;
    StmtPtr body;
};

/** Top-level item stores exactly one declaration or function in source order. /
 * 顶层项依源码次序只保存声明或函数之一。 */
struct TopLevel {
    SourceRange range;
    std::optional<Declaration> declaration;
    std::optional<Function> function;
};

/** Translation unit. / 翻译单元。 */
struct Program {
    std::vector<TopLevel> items;
};

} // namespace sysy
