#include "compiler/optimize.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

/// 中文：Release 也必须检查 IR 不变量；English: Keep IR checks active in Release builds.
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                 \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

/** 中文：构造不借用源码或语义表的最小 SSA 指令；English: Build a minimal owned SSA instruction. */
sysy::Instruction
instruction(
    sysy::IrOp op,
    sysy::IrType type,
    sysy::ValueId dst,
    std::vector<sysy::ValueId> args = {},
    std::int64_t imm = 0
) {
    sysy::Instruction result;
    result.op = op;
    result.type = type;
    result.dst = dst;
    result.args = std::move(args);
    result.imm = imm;
    return result;
}

/**
 * 中文：构造有一条定义路径和一条未定义路径的菱形 CFG。未定义路径仅用于结构测试，
 * 运行时测试只能调用条件为真的定义路径。
 * English: Build a diamond CFG with one defined and one indeterminate path.
 * Runtime tests may exercise only the defined (true) path.
 */
sysy::ModuleIR
diamond() {
    using namespace sysy;
    ModuleIR module;
    FunctionIR function;
    function.name = "choose_defined";
    function.return_type = IrType::I32;
    function.parameters = {IrType::I32};
    const auto entry = function.new_block();
    const auto yes = function.new_block();
    const auto no = function.new_block();
    const auto join = function.new_block();
    function.entry = entry;

    const auto condition = function.new_value(IrType::I32);
    const auto slot = function.new_value(IrType::Ptr);
    const auto seven = function.new_value(IrType::I32);
    const auto loaded = function.new_value(IrType::I32);
    function.blocks[entry].instructions.push_back(
        instruction(IrOp::Param, IrType::I32, condition, {}, 0)
    );
    auto alloca = instruction(IrOp::Alloca, IrType::Ptr, slot, {}, 4);
    alloca.aux = 4;
    function.blocks[entry].instructions.push_back(std::move(alloca));
    function.blocks[entry].terminator = {TermKind::Branch, condition, yes, no};

    function.blocks[yes].instructions.push_back(
        instruction(IrOp::ConstI32, IrType::I32, seven, {}, 7)
    );
    function.blocks[yes].instructions.push_back(
        instruction(IrOp::Store, IrType::Void, no_value, {slot, seven})
    );
    function.blocks[yes].terminator = {TermKind::Jump, no_value, join, no_block};
    function.blocks[no].terminator = {TermKind::Jump, no_value, join, no_block};
    function.blocks[join].instructions.push_back(
        instruction(IrOp::Load, IrType::I32, loaded, {slot})
    );
    function.blocks[join].terminator = {TermKind::Return, loaded, no_block, no_block};
    module.functions.push_back(std::move(function));
    return module;
}

/**
 * 中文：证明未定义前驱不会阻止可提升标量的 mem2reg，也不会被 SCCP 误折叠为常量。
 * English: Prove an undefined predecessor neither blocks scalar promotion nor
 * lets SCCP replace the resulting value with a concrete constant.
 */
void
check_promotion(int level) {
    using namespace sysy;
    auto module = diamond();
    CHECK(verify_ir(module).empty());
    CHECK(optimize(module, level).empty());
    CHECK(verify_ir(module).empty());

    const auto& function = module.functions.front();
    ValueId undef = no_value;
    ValueId phi = no_value;
    for (const auto& block : function.blocks) {
        for (const auto& item : block.instructions) {
            CHECK(item.op != IrOp::Alloca);
            CHECK(item.op != IrOp::Load);
            CHECK(item.op != IrOp::Store);
            if (item.op == IrOp::Undef) {
                CHECK(item.type == IrType::I32);
                CHECK(undef == no_value);
                undef = item.dst;
            }
            if (item.op == IrOp::Phi) {
                CHECK(phi == no_value);
                phi = item.dst;
                CHECK(item.incoming.size() == 2);
            }
        }
    }
    CHECK(undef != no_value);
    CHECK(phi != no_value);
    bool has_undef_edge = false;
    for (const auto& block : function.blocks) {
        for (const auto& item : block.instructions) {
            if (item.op != IrOp::Phi)
                continue;
            for (const auto& [predecessor, value] : item.incoming) {
                (void)predecessor;
                has_undef_edge |= value == undef;
            }
        }
        if (block.terminator.kind == TermKind::Return)
            CHECK(block.terminator.value == phi);
    }
    CHECK(has_undef_edge);
}

} // namespace

/// 中文：分别检验 O1/O2 的转换和 SCCP 交互；English: Check O1/O2 promotion and SCCP interaction.
int
main() {
    check_promotion(1);
    check_promotion(2);
    return 0;
}
