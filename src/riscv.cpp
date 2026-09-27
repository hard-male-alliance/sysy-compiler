#include "compiler/riscv.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sysy {
namespace {

/** ABI location for one source-order parameter. / 单个源码顺序形参的 ABI 位置。 */
struct ArgLoc {
    enum class Kind { IntReg, FloatReg, Stack } kind;
    unsigned index;
};

/** Allocate the two register classes independently, with f32 integer fallback. /
 * 独立分配两类寄存器，浮点寄存器耗尽后回退到整型通道。 */
std::vector<ArgLoc>
plan_args(const std::vector<IrType>& types) {
    std::vector<ArgLoc> result;
    unsigned ints = 0, floats = 0, stack = 0;
    for (IrType type : types) {
        if (type == IrType::F32 && floats < 8)
            result.push_back({ArgLoc::Kind::FloatReg, floats++});
        else if (ints < 8)
            result.push_back({ArgLoc::Kind::IntReg, ints++});
        else
            result.push_back({ArgLoc::Kind::Stack, stack++});
    }
    return result;
}

/// Validate and round a frame offset to power-of-two alignment. / 校验并按二次幂对齐栈帧偏移。
std::int64_t
align_to(std::int64_t value, std::int64_t alignment) {
    if (alignment <= 0
        || (alignment & (alignment - 1)) != 0
        || value > std::numeric_limits<std::int64_t>::max() - alignment)
        throw std::runtime_error("invalid or excessive stack alignment");
    return (value + alignment - 1) & -alignment;
}

/** Per-function stack layout. Outgoing ABI arguments start at sp+0. / 单函数栈布局；传出 ABI 参数从
 * sp+0 开始。 */
struct Frame {
    std::vector<std::int64_t> home;
    std::vector<std::int64_t> object;
    std::vector<std::int64_t> phi_temp;
    std::int64_t size = 0;
    std::int64_t ra = 0;
};

/// Reserve outgoing slots, SSA homes, objects, Phi staging, and saved ra. / 预留传出参数、SSA
/// 驻留、对象、Phi 暂存及返回地址。
Frame
plan_frame(const FunctionIR& fn) {
    Frame f;
    std::size_t max_stack = 0;
    for (const auto& block : fn.blocks)
        for (const auto& ins : block.instructions) {
            if (ins.op != IrOp::Call)
                continue;
            std::vector<IrType> types;
            for (ValueId arg : ins.args) {
                if (arg >= fn.value_types.size())
                    throw std::runtime_error("invalid call operand");
                types.push_back(fn.value_types[arg]);
            }
            for (auto loc : plan_args(types))
                if (loc.kind == ArgLoc::Kind::Stack)
                    max_stack = std::max(max_stack, std::size_t(loc.index + 1));
        }
    std::int64_t pos = static_cast<std::int64_t>(max_stack) * 8;
    f.home.resize(fn.value_types.size());
    f.object.resize(fn.value_types.size(), -1);
    f.phi_temp.resize(fn.value_types.size(), -1);
    for (std::size_t i = 0; i < fn.value_types.size(); ++i) {
        pos = align_to(pos, 8);
        f.home[i] = pos;
        pos += 8;
    }
    for (const auto& block : fn.blocks)
        for (const auto& ins : block.instructions) {
            if (ins.dst == no_value)
                continue;
            if (ins.dst >= f.home.size())
                throw std::runtime_error("invalid destination ID");
            if (ins.op == IrOp::Alloca) {
                auto alignment = std::max<std::int64_t>(1, ins.aux);
                if (alignment > 16)
                    throw std::runtime_error("unsupported alloca alignment above 16");
                pos = align_to(pos, alignment);
                f.object[ins.dst] = pos;
                if (ins.imm < 0 || ins.imm > std::numeric_limits<std::int64_t>::max() - pos)
                    throw std::runtime_error("invalid alloca size");
                pos += std::max<std::int64_t>(1, ins.imm);
            } else if (ins.op == IrOp::Phi) {
                pos = align_to(pos, 8);
                f.phi_temp[ins.dst] = pos;
                pos += 8;
            }
        }
    pos = align_to(pos, 8);
    f.ra = pos;
    f.size = align_to(pos + 8, 16);
    return f;
}

/// Test signed I-type immediate range. / 检查带符号 I 类立即数范围。
bool
fits_imm(std::int64_t n) {
    return n >= -2048 && n <= 2047;
}

/// Permit only SysY C identifiers in assembler symbol positions. / 汇编符号位置仅允许 SysY 的 C
/// 标识符。
void
check_symbol(std::string_view name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_'))
        throw std::runtime_error("invalid assembly symbol");
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            throw std::runtime_error("invalid assembly symbol");
}

/** Stack-homed emitter uses only caller-saved scratch registers. /
 * 栈驻留发射器仅使用调用者保存的暂存寄存器。 */
class FunctionEmitter {
public:
    FunctionEmitter(std::ostringstream& out, const FunctionIR& fn, std::size_t ordinal)
        : out_(out),
          fn_(fn),
          ordinal_(ordinal),
          frame_(plan_frame(fn)),
          params_(plan_args(fn.parameters)) {}

    /// Emit one complete function and its ELF size directive. / 发射完整函数及 ELF 大小指令。
    void
    emit() {
        check_symbol(fn_.name);
        out_
            << ".text\n.align 2\n.globl "
            << fn_.name
            << "\n.type "
            << fn_.name
            << ", @function\n"
            << fn_.name
            << ":\n";
        if (fits_imm(-frame_.size))
            out_ << "  addi sp, sp, " << -frame_.size << '\n';
        else
            out_ << "  li t5, " << frame_.size << "\n  sub sp, sp, t5\n";
        mem("sd", "ra", frame_.ra);
        save_params();
        for (BlockId id = 0; id < fn_.blocks.size(); ++id) {
            out_ << label(id) << ":\n";
            for (const auto& ins : fn_.blocks[id].instructions)
                emit_instruction(ins);
            emit_terminator(id, fn_.blocks[id].terminator);
        }
        out_ << ".size " << fn_.name << ", .-" << fn_.name << '\n';
    }

private:
    std::ostringstream& out_;
    const FunctionIR& fn_;
    std::size_t ordinal_;
    Frame frame_;
    std::vector<ArgLoc> params_;

    /// Create a translation-unit-unique private block label. / 生成翻译单元内唯一的私有块标签。
    std::string
    label(BlockId id) const {
        return ".Lsysy_" + std::to_string(ordinal_) + "_" + std::to_string(id);
    }

    /// Read a verified value type with a defensive bounds check. / 读取值类型并防御性检查越界。
    IrType
    type(ValueId id) const {
        if (id >= fn_.value_types.size())
            throw std::runtime_error("invalid value ID");
        return fn_.value_types[id];
    }

    /// Materialize large offsets through t5; t5 must not hold a live operand. / 用 t5
    /// 构造大偏移；t5 不可存放活跃操作数。
    void
    mem(std::string_view op, std::string_view reg, std::int64_t off, std::string_view base = "sp") {
        if (fits_imm(off))
            out_ << "  " << op << ' ' << reg << ", " << off << '(' << base << ")\n";
        else {
            out_ << "  li t5, " << off << "\n  add t5, " << base << ", t5\n";
            out_ << "  " << op << ' ' << reg << ", 0(t5)\n";
        }
    }

    /// Materialize an sp-relative object address. / 构造相对 sp 的对象地址。
    void
    address(std::string_view reg, std::int64_t off) {
        if (fits_imm(off))
            out_ << "  addi " << reg << ", sp, " << off << '\n';
        else
            out_ << "  li " << reg << ", " << off << "\n  add " << reg << ", sp, " << reg << '\n';
    }

    /// Reload an SSA home using its machine type. / 按机器类型重新装载 SSA 驻留值。
    void
    load(ValueId id, std::string_view reg = "t0") {
        auto ty = type(id);
        if (ty == IrType::F32)
            mem("flw", reg, frame_.home[id]);
        else
            mem(ty == IrType::I32 ? "lw" : "ld", reg, frame_.home[id]);
    }

    /// Store an SSA result in its canonical home. / 将 SSA 结果写入规范驻留位置。
    void
    save(ValueId id, std::string_view reg = "t0") {
        auto ty = type(id);
        if (ty == IrType::F32)
            mem("fsw", reg, frame_.home[id]);
        else
            mem(ty == IrType::I32 ? "sw" : "sd", reg, frame_.home[id]);
    }

    /// Snapshot incoming ABI locations before any generated call. / 在生成的调用之前保存传入 ABI
    /// 参数。
    void
    save_params() {
        for (const auto& block : fn_.blocks)
            for (const auto& ins : block.instructions) {
                if (ins.op != IrOp::Param)
                    continue;
                if (ins.imm < 0 || static_cast<std::size_t>(ins.imm) >= params_.size())
                    throw std::runtime_error("invalid parameter index");
                auto loc = params_[static_cast<std::size_t>(ins.imm)];
                auto ty = type(ins.dst);
                if (loc.kind == ArgLoc::Kind::FloatReg)
                    save(ins.dst, "fa" + std::to_string(loc.index));
                else if (loc.kind == ArgLoc::Kind::IntReg) {
                    if (ty == IrType::F32) {
                        out_ << "  fmv.w.x ft0, a" << loc.index << '\n';
                        save(ins.dst, "ft0");
                    } else
                        save(ins.dst, "a" + std::to_string(loc.index));
                } else {
                    auto off = frame_.size + static_cast<std::int64_t>(loc.index) * 8;
                    if (ty == IrType::F32) {
                        mem("flw", "ft0", off);
                        save(ins.dst, "ft0");
                    } else {
                        mem(ty == IrType::I32 ? "lw" : "ld", "t0", off);
                        save(ins.dst);
                    }
                }
            }
    }

    /// Move source-order arguments into planned ABI locations and call. / 将源码顺序实参搬入预定
    /// ABI 位置后调用。
    void
    emit_call(const Instruction& ins) {
        check_symbol(ins.symbol);
        std::vector<IrType> types;
        for (ValueId arg : ins.args)
            types.push_back(type(arg));
        auto locs = plan_args(types);
        for (std::size_t i = 0; i < ins.args.size(); ++i) {
            auto loc = locs[i];
            auto ty = types[i];
            if (loc.kind == ArgLoc::Kind::FloatReg) {
                load(ins.args[i], "ft0");
                out_ << "  fmv.s fa" << loc.index << ", ft0\n";
            } else if (loc.kind == ArgLoc::Kind::IntReg) {
                if (ty == IrType::F32) {
                    load(ins.args[i], "ft0");
                    out_ << "  fmv.x.w a" << loc.index << ", ft0\n";
                } else
                    load(ins.args[i], "a" + std::to_string(loc.index));
            } else {
                if (ty == IrType::F32) {
                    load(ins.args[i], "ft0");
                    mem("fsw", "ft0", static_cast<std::int64_t>(loc.index) * 8);
                } else {
                    load(ins.args[i]);
                    mem(ty == IrType::I32 ? "sd" : "sd",
                        "t0",
                        static_cast<std::int64_t>(loc.index) * 8);
                }
            }
        }
        out_ << "  call " << ins.symbol << '\n';
        if (ins.dst != no_value)
            save(ins.dst, ins.type == IrType::F32 ? "fa0" : "a0");
    }

    /// Lower one typed IR instruction without hidden live registers. / 降低一条带类型 IR
    /// 指令且不保留隐式活跃寄存器。
    void
    emit_instruction(const Instruction& ins) {
        auto arg = [&](std::size_t i) -> ValueId {
            if (i >= ins.args.size())
                throw std::runtime_error("missing instruction operand");
            return ins.args[i];
        };
        switch (ins.op) {
        case IrOp::Param:
        case IrOp::Phi:
            break;
        case IrOp::ConstI32:
            out_ << "  li t0, " << static_cast<std::int32_t>(ins.imm) << '\n';
            save(ins.dst);
            break;
        case IrOp::ConstF32:
            out_ << "  li t0, " << static_cast<std::uint32_t>(ins.imm) << "\n  fmv.w.x ft0, t0\n";
            save(ins.dst, "ft0");
            break;
        case IrOp::Alloca:
            if (frame_.object.at(ins.dst) < 0)
                throw std::runtime_error("missing stack object");
            address("t0", frame_.object[ins.dst]);
            save(ins.dst);
            break;
        case IrOp::GlobalAddr:
            check_symbol(ins.symbol);
            out_ << "  la t0, " << ins.symbol << '\n';
            save(ins.dst);
            break;
        case IrOp::PtrAdd:
            load(arg(0));
            load(arg(1), "t1");
            out_ << "  add t0, t0, t1\n";
            save(ins.dst);
            break;
        case IrOp::Load:
            load(arg(0));
            if (ins.type == IrType::F32) {
                mem("flw", "ft0", 0, "t0");
                save(ins.dst, "ft0");
            } else {
                mem(ins.type == IrType::I32 ? "lw" : "ld", "t1", 0, "t0");
                save(ins.dst, "t1");
            }
            break;
        case IrOp::Store:
            load(arg(0));
            if (type(arg(1)) == IrType::F32) {
                load(arg(1), "ft0");
                mem("fsw", "ft0", 0, "t0");
            } else {
                load(arg(1), "t1");
                mem(type(arg(1)) == IrType::I32 ? "sw" : "sd", "t1", 0, "t0");
            }
            break;
        case IrOp::Add:
        case IrOp::Sub:
        case IrOp::Mul:
        case IrOp::Div:
        case IrOp::Rem:
            emit_arithmetic(ins);
            break;
        case IrOp::Cmp:
            emit_comparison(ins);
            break;
        case IrOp::IToF:
            load(arg(0));
            out_ << "  fcvt.s.w ft0, t0\n";
            save(ins.dst, "ft0");
            break;
        case IrOp::FToI:
            load(arg(0), "ft0");
            out_ << "  fcvt.w.s t0, ft0, rtz\n";
            save(ins.dst);
            break;
        case IrOp::Call:
            emit_call(ins);
            break;
        }
    }

    /// Choose word or single-precision arithmetic from the result type. /
    /// 根据结果类型选取字或单精度算术。
    void
    emit_arithmetic(const Instruction& ins) {
        if (ins.args.size() != 2)
            throw std::runtime_error("binary arithmetic needs two operands");
        if (ins.type == IrType::F32) {
            load(ins.args[0], "ft0");
            load(ins.args[1], "ft1");
            const char* op = ins.op == IrOp::Add   ? "fadd.s"
                             : ins.op == IrOp::Sub ? "fsub.s"
                             : ins.op == IrOp::Mul ? "fmul.s"
                             : ins.op == IrOp::Div ? "fdiv.s"
                                                   : nullptr;
            if (!op)
                throw std::runtime_error("float remainder is not supported");
            out_ << "  " << op << " ft0, ft0, ft1\n";
            save(ins.dst, "ft0");
        } else {
            load(ins.args[0]);
            load(ins.args[1], "t1");
            const bool word = ins.type == IrType::I32;
            const char* op = ins.op == IrOp::Add   ? (word ? "addw" : "add")
                             : ins.op == IrOp::Sub ? (word ? "subw" : "sub")
                             : ins.op == IrOp::Mul ? (word ? "mulw" : "mul")
                             : ins.op == IrOp::Div ? (word ? "divw" : "div")
                                                   : "remw";
            out_ << "  " << op << " t0, t0, t1\n";
            save(ins.dst);
        }
    }

    /// Implement ordered relations and NaN-aware float inequality. / 实现有序关系与感知 NaN
    /// 的浮点不等式。
    void
    emit_comparison(const Instruction& ins) {
        if (ins.args.size() != 2 || ins.imm < 0 || ins.imm > static_cast<int>(CmpPred::Ge))
            throw std::runtime_error("invalid comparison");
        auto pred = static_cast<CmpPred>(ins.imm);
        if (type(ins.args[0]) == IrType::F32) {
            load(ins.args[0], "ft0");
            load(ins.args[1], "ft1");
            switch (pred) {
            case CmpPred::Eq:
            case CmpPred::Ne:
                out_ << "  feq.s t0, ft0, ft1\n";
                break;
            case CmpPred::Lt:
                out_ << "  flt.s t0, ft0, ft1\n";
                break;
            case CmpPred::Le:
                out_ << "  fle.s t0, ft0, ft1\n";
                break;
            case CmpPred::Gt:
                out_ << "  flt.s t0, ft1, ft0\n";
                break;
            case CmpPred::Ge:
                out_ << "  fle.s t0, ft1, ft0\n";
                break;
            }
            if (pred == CmpPred::Ne)
                out_ << "  xori t0, t0, 1\n";
        } else {
            load(ins.args[0]);
            load(ins.args[1], "t1");
            switch (pred) {
            case CmpPred::Eq:
                out_ << "  sub t0, t0, t1\n  seqz t0, t0\n";
                break;
            case CmpPred::Ne:
                out_ << "  sub t0, t0, t1\n  snez t0, t0\n";
                break;
            case CmpPred::Lt:
                out_ << "  slt t0, t0, t1\n";
                break;
            case CmpPred::Le:
                out_ << "  slt t0, t1, t0\n  xori t0, t0, 1\n";
                break;
            case CmpPred::Gt:
                out_ << "  slt t0, t1, t0\n";
                break;
            case CmpPred::Ge:
                out_ << "  slt t0, t0, t1\n  xori t0, t0, 1\n";
                break;
            }
        }
        save(ins.dst);
    }

    /// Stage all Phi sources before committing any destination. / 写任一 Phi 目标前暂存所有来源。
    void
    edge_moves(BlockId from, BlockId to) {
        if (to >= fn_.blocks.size())
            throw std::runtime_error("invalid branch target");
        // 中文：先暂存全部来源，再写目标，防止循环 Phi 的并行赋值互相覆盖。
        // English: Stage every source before committing destinations; loop Phi assignments are
        // parallel.
        for (const auto& ins : fn_.blocks[to].instructions) {
            if (ins.op != IrOp::Phi)
                break;
            auto it = std::find_if(ins.incoming.begin(), ins.incoming.end(), [from](const auto& p) {
                return p.first == from;
            });
            if (it == ins.incoming.end())
                throw std::runtime_error("missing Phi incoming edge");
            auto ty = type(it->second);
            if (ty == IrType::F32) {
                load(it->second, "ft0");
                mem("fsw", "ft0", frame_.phi_temp.at(ins.dst));
            } else {
                load(it->second);
                mem(ty == IrType::I32 ? "sw" : "sd", "t0", frame_.phi_temp.at(ins.dst));
            }
        }
        for (const auto& ins : fn_.blocks[to].instructions) {
            if (ins.op != IrOp::Phi)
                break;
            auto ty = type(ins.dst);
            if (ty == IrType::F32) {
                mem("flw", "ft0", frame_.phi_temp.at(ins.dst));
                save(ins.dst, "ft0");
            } else {
                mem(ty == IrType::I32 ? "lw" : "ld", "t0", frame_.phi_temp.at(ins.dst));
                save(ins.dst);
            }
        }
    }

    /// Resolve edge-local Phi moves and transfer control. / 解析边局部 Phi 搬运并转移控制流。
    void
    emit_terminator(BlockId from, const Terminator& term) {
        switch (term.kind) {
        case TermKind::None:
            throw std::runtime_error("unterminated IR block");
        case TermKind::Jump:
            edge_moves(from, term.yes);
            out_ << "  j " << label(term.yes) << '\n';
            break;
        case TermKind::Branch: {
            load(term.value);
            auto false_edge =
                ".Lsysy_" + std::to_string(ordinal_) + "_edge_" + std::to_string(from);
            out_ << "  beqz t0, " << false_edge << '\n';
            edge_moves(from, term.yes);
            out_ << "  j " << label(term.yes) << '\n';
            out_ << false_edge << ":\n";
            edge_moves(from, term.no);
            out_ << "  j " << label(term.no) << '\n';
            break;
        }
        case TermKind::Return:
            if (term.value != no_value)
                load(term.value, type(term.value) == IrType::F32 ? "fa0" : "a0");
            mem("ld", "ra", frame_.ra);
            if (fits_imm(frame_.size))
                out_ << "  addi sp, sp, " << frame_.size << '\n';
            else
                out_ << "  li t5, " << frame_.size << "\n  add sp, sp, t5\n";
            out_ << "  ret\n";
            break;
        }
    }
};

/// Select ELF data sections while preserving raw initializer words. / 选择 ELF
/// 数据段并保留初值原始位型。
void
emit_globals(std::ostringstream& out, const ModuleIR& module) {
    for (const auto& g : module.globals) {
        check_symbol(g.name);
        bool zero =
            std::all_of(g.words.begin(), g.words.end(), [](std::uint32_t x) { return x == 0; });
        out << (g.is_const ? ".section .rodata\n" : zero ? ".bss\n" : ".data\n");
        out << ".align 2\n.globl " << g.name << "\n.type " << g.name << ", @object\n";
        out << ".size " << g.name << ", " << g.words.size() * 4 << '\n' << g.name << ":\n";
        if (zero)
            out << "  .zero " << g.words.size() * 4 << '\n';
        else
            for (auto word : g.words)
                out << "  .word " << word << '\n';
    }
}

} // namespace

/// Buffer codegen to keep semantic errors from publishing partial output. /
/// 缓冲代码生成，避免语义错误时发布半成品。
std::expected<void, std::string>
emit_rv64(const ModuleIR& module, std::ostream& out) {
    try {
        std::ostringstream assembly;
        assembly
            << ".option nopic\n.attribute arch, \"rv64imafdc\"\n.attribute unaligned_access, 0\n";
        emit_globals(assembly, module);
        for (std::size_t i = 0; i < module.functions.size(); ++i)
            FunctionEmitter(assembly, module.functions[i], i).emit();
        assembly << ".section .note.GNU-stack,\"\",@progbits\n";
        out << assembly.str();
        if (!out)
            return std::unexpected("assembly output stream failed");
        return {};
    } catch (const std::exception& ex) {
        return std::unexpected(ex.what());
    }
}

} // namespace sysy
