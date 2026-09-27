#pragma once

#include "compiler/ast.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace sysy {

/** Stable index of an SSA value. / SSA 值的稳定索引。 */
using ValueId = std::uint32_t;
/** Stable index of a basic block. / 基本块的稳定索引。 */
using BlockId = std::uint32_t;
inline constexpr ValueId no_value = std::numeric_limits<ValueId>::max();
inline constexpr BlockId no_block = std::numeric_limits<BlockId>::max();

/** Machine-relevant IR type; Ptr is internal and points to scalar storage. / 与机器相关的 IR
 * 类型；Ptr 仅供内部使用，指向标量存储。 */
enum class IrType { I32, F32, Ptr, Void };

/** Comparison predicate; floating Ne must treat NaN as true. / 比较谓词；浮点 Ne 必须将 NaN
 * 视为真。 */
enum class CmpPred { Eq, Ne, Lt, Le, Gt, Ge };

/** Three-address operation; arithmetic opcode is shared by i32 and f32. /
 * 三地址操作；整型与浮点型共用算术操作码。 */
enum class IrOp {
    ConstI32,
    ConstF32,
    Undef,
    Param,
    Alloca,
    GlobalAddr,
    PtrAdd,
    Load,
    Store,
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Cmp,
    IToF,
    FToI,
    Call,
    Phi
};

/**
 * One SSA definition or side effect. `args` are source-order operands; `imm` is
 * integer literal, float bits, parameter index, alloca byte size, or CmpPred.
 * Undef defines an indeterminate i32/f32 value: it is never a constant and
 * its backend home is deliberately not initialized. / Undef 定义未定的 i32/f32
 * 值：绝不是常量，后端也刻意不初始化其驻留栈槽。
 * `aux` is alloca alignment. PtrAdd's offset is in BYTES. Phi uses incoming,
 * not args. / 一条 SSA 定义或副作用；args 按源码顺序排列；imm 存储整数、浮点位型、
 * 形参索引、分配字节数或比较谓词；aux 是分配对齐。PtrAdd 偏移以字节计。
 * Phi 使用 incoming 而非 args。
 */
struct Instruction {
    IrOp op = IrOp::ConstI32;
    IrType type = IrType::Void;
    ValueId dst = no_value;
    std::vector<ValueId> args;
    std::int64_t imm = 0;
    std::uint32_t aux = 0;
    std::string symbol;
    std::vector<std::pair<BlockId, ValueId>> incoming;
    SourceRange range;
};

/** Distinct control-flow exit; true edge is yes, false edge is no. / 独立控制流出口；真边为
 * yes，假边为 no。 */
enum class TermKind { None, Jump, Branch, Return };

/** Block terminator. Return with no value represents void return. / 基本块终结符；无值 Return 表示
 * void 返回。 */
struct Terminator {
    TermKind kind = TermKind::None;
    ValueId value = no_value;
    BlockId yes = no_block;
    BlockId no = no_block;
};

/** IR basic block; Phi instructions must precede all non-Phi instructions. / IR 基本块；Phi
 * 指令必须排在其他指令之前。 */
struct BasicBlock {
    std::vector<Instruction> instructions;
    Terminator terminator;
};

/** A function owns stable block/value IDs and its source-order ABI signature. /
 * 函数拥有稳定块与值编号，以及按源码顺序排列的 ABI 签名。 */
struct FunctionIR {
    std::string name;
    IrType return_type = IrType::Void;
    std::vector<IrType> parameters;
    std::vector<BasicBlock> blocks;
    BlockId entry = no_block;
    std::vector<IrType> value_types;

    /** Allocate a stable SSA value ID. / 分配稳定的 SSA 值编号。 */
    [[nodiscard]] ValueId
    new_value(IrType type) {
        const auto id = static_cast<ValueId>(value_types.size());
        value_types.push_back(type);
        return id;
    }

    /** Allocate a stable basic-block ID. / 分配稳定的基本块编号。 */
    [[nodiscard]] BlockId
    new_block() {
        const auto id = static_cast<BlockId>(blocks.size());
        blocks.emplace_back();
        return id;
    }
};

/** Global scalar or array; words hold row-major 32-bit int/float bit patterns. /
 * 全局标量或数组；words 按行优先顺序保存 32 位整数或浮点位型。 */
struct GlobalIR {
    std::string name;
    IrType element_type = IrType::I32;
    std::vector<std::uint32_t> words;
    bool is_const = false;
};

/** One compilation unit; every referenced function is defined or a runtime symbol. /
 * 一个编译单元；每个引用的函数均已定义或为运行库符号。 */
struct ModuleIR {
    std::vector<GlobalIR> globals;
    std::vector<FunctionIR> functions;
};

/** Verify index, type, Phi ordering, and CFG structural invariants. / 验证索引、类型、Phi 顺序及
 * CFG 结构不变量。 */
[[nodiscard]] std::vector<std::string> verify_ir(const ModuleIR& module);

} // namespace sysy
