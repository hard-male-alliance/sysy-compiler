#include "compiler/optimize.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace sysy {
namespace {

using Blocks = std::vector<std::vector<BlockId>>;

/** Build successor lists from the unique terminator. / 从唯一终结符构造后继表。 */
Blocks
successors(const FunctionIR& fn) {
    Blocks out(fn.blocks.size());
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        const auto& term = fn.blocks[b].terminator;
        if (term.kind == TermKind::Jump || term.kind == TermKind::Branch)
            out[b].push_back(term.yes);
        if (term.kind == TermKind::Branch && term.no != term.yes)
            out[b].push_back(term.no);
    }
    return out;
}

/** Invert a valid CFG's edges. / 反转有效 CFG 的边。 */
Blocks
predecessors(const Blocks& succ) {
    Blocks pred(succ.size());
    for (BlockId b = 0; b < succ.size(); ++b)
        for (const BlockId s : succ[b])
            if (s < succ.size())
                pred[s].push_back(b);
    return pred;
}

/** Entry-reachable blocks, independent of vector order. / 从入口可达的块，不依赖向量顺序。 */
std::vector<bool>
reachable(const FunctionIR& fn, const Blocks& succ) {
    std::vector<bool> seen(fn.blocks.size());
    if (fn.entry >= fn.blocks.size())
        return seen;
    std::vector<BlockId> todo{fn.entry};
    seen[fn.entry] = true;
    while (!todo.empty()) {
        const auto b = todo.back();
        todo.pop_back();
        for (const auto s : succ[b])
            if (s < seen.size() && !seen[s]) {
                seen[s] = true;
                todo.push_back(s);
            }
    }
    return seen;
}

/** Remove dead blocks and rewrite every block-valued operand atomically. /
 * 原子地删除死块并重写所有块编号操作数。 */
bool
remove_unreachable(FunctionIR& fn) {
    const auto live = reachable(fn, successors(fn));
    if (std::ranges::all_of(live, [](bool x) { return x; }))
        return false;
    std::vector<BlockId> map(fn.blocks.size(), no_block);
    std::vector<BasicBlock> blocks;
    blocks.reserve(fn.blocks.size());
    for (BlockId b = 0; b < fn.blocks.size(); ++b)
        if (live[b]) {
            map[b] = static_cast<BlockId>(blocks.size());
            blocks.push_back(std::move(fn.blocks[b]));
        }
    for (auto& block : blocks) {
        auto& term = block.terminator;
        if (term.kind == TermKind::Jump || term.kind == TermKind::Branch)
            term.yes = map[term.yes];
        if (term.kind == TermKind::Branch)
            term.no = map[term.no];
        for (auto& inst : block.instructions)
            if (inst.op == IrOp::Phi) {
                std::erase_if(inst.incoming, [&](const auto& edge) {
                    return edge.first >= map.size() || map[edge.first] == no_block;
                });
                for (auto& edge : inst.incoming)
                    edge.first = map[edge.first];
            }
    }
    fn.entry = map[fn.entry];
    fn.blocks = std::move(blocks);
    return true;
}

/**
 * Immediate-dominator tree plus a query matrix for verification. The RPO/intersect
 * iteration follows the algorithmic idea of Cooper, Harvey, and Kennedy (2001),
 * https://hipersoft.cs.rice.edu/grads/publications/dom14.pdf ; this implementation
 * is independent, not copied source. / 直接支配树及用于验证的查询矩阵。RPO/intersect
 * 迭代采用 Cooper、Harvey 与 Kennedy（2001）的算法思想；实现为独立编写。
 */
struct Dominators {
    std::vector<std::vector<bool>> dom;
    std::vector<BlockId> idom;
    Blocks children;
    Blocks frontier;
};

Dominators
dominators(const FunctionIR& fn, const Blocks& pred) {
    const auto n = fn.blocks.size();
    Dominators d{
        std::vector<std::vector<bool>>(n, std::vector<bool>(n)),
        std::vector<BlockId>(n, no_block),
        Blocks(n),
        Blocks(n)
    };
    const auto succ = successors(fn);
    std::vector<bool> seen(n);
    std::vector<BlockId> postorder;
    std::vector<std::pair<BlockId, std::size_t>> walk{{fn.entry, 0}};
    seen[fn.entry] = true;
    while (!walk.empty()) {
        auto& [b, next] = walk.back();
        if (next == succ[b].size()) {
            postorder.push_back(b);
            walk.pop_back();
            continue;
        }
        const auto s = succ[b][next++];
        if (s < n && !seen[s]) {
            seen[s] = true;
            walk.emplace_back(s, 0);
        }
    }
    std::vector<BlockId> rpo(postorder.rbegin(), postorder.rend());
    std::vector<std::size_t> rank(n, n);
    for (std::size_t i = 0; i < rpo.size(); ++i)
        rank[rpo[i]] = i;
    d.idom[fn.entry] = fn.entry;
    const auto intersect = [&](BlockId a, BlockId b) {
        while (a != b) {
            while (rank[a] > rank[b])
                a = d.idom[a];
            while (rank[b] > rank[a])
                b = d.idom[b];
        }
        return a;
    };
    bool changed;
    do {
        changed = false;
        for (std::size_t i = 1; i < rpo.size(); ++i) {
            const auto b = rpo[i];
            BlockId next = no_block;
            for (const auto p : pred[b])
                if (d.idom[p] != no_block)
                    next = next == no_block ? p : intersect(next, p);
            if (next != no_block && d.idom[b] != next) {
                d.idom[b] = next;
                changed = true;
            }
        }
    } while (changed);
    for (const auto b : rpo) {
        for (auto ancestor = b;; ancestor = d.idom[ancestor]) {
            d.dom[b][ancestor] = true;
            if (ancestor == fn.entry)
                break;
        }
        if (b != fn.entry)
            d.children[d.idom[b]].push_back(b);
    }
    for (const auto b : rpo)
        if (pred[b].size() >= 2)
            for (auto p : pred[b])
                if (seen[p]) {
                    while (p != d.idom[b] && p != no_block) {
                        d.frontier[p].push_back(b);
                        p = d.idom[p];
                    }
                }
    for (auto& frontier : d.frontier) {
        std::ranges::sort(frontier);
        frontier.erase(std::unique(frontier.begin(), frontier.end()), frontier.end());
    }
    return d;
}

/** Resolve transitive substitutions produced by promotion or CSE. / 解析提升或 CSE
 * 产生的传递性替换。 */
ValueId
resolve(ValueId id, const std::vector<ValueId>& replacement) {
    if (id == no_value)
        return id;
    while (id < replacement.size() && replacement[id] != no_value && replacement[id] != id)
        id = replacement[id];
    return id;
}

/** Update all uses, including edge uses and terminators. / 更新普通使用、边使用及终结符使用。 */
void
rewrite_uses(FunctionIR& fn, const std::vector<ValueId>& replacement) {
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            for (auto& arg : inst.args)
                arg = resolve(arg, replacement);
            for (auto& edge : inst.incoming)
                edge.second = resolve(edge.second, replacement);
        }
        block.terminator.value = resolve(block.terminator.value, replacement);
    }
}

/** A slot is promoted only when its address never escapes and every load is definitely initialized.
 * / 只有地址不逃逸且每次加载必已初始化才提升栈槽。 */
bool
promotable(const FunctionIR& fn, ValueId slot, const Blocks& pred) {
    if (fn.value_types[slot] != IrType::Ptr)
        return false;
    bool any = false;
    for (const auto& block : fn.blocks) {
        if (block.terminator.value == slot)
            return false;
        for (const auto& inst : block.instructions) {
            for (std::size_t i = 0; i < inst.args.size(); ++i)
                if (inst.args[i] == slot) {
                    if (!((inst.op == IrOp::Load && i == 0 && inst.args.size() == 1)
                          || (inst.op == IrOp::Store && i == 0 && inst.args.size() == 2)))
                        return false;
                    any = true;
                }
            for (const auto& edge : inst.incoming)
                if (edge.second == slot)
                    return false;
        }
    }
    if (!any)
        return true;
    std::vector<bool> in(fn.blocks.size()), out(fn.blocks.size());
    bool changed;
    do {
        changed = false;
        for (BlockId b = 0; b < fn.blocks.size(); ++b) {
            bool assigned = b != fn.entry && !pred[b].empty();
            if (b != fn.entry)
                for (auto p : pred[b])
                    assigned = assigned && out[p];
            const bool next_in = assigned;
            for (const auto& inst : fn.blocks[b].instructions)
                if (inst.op == IrOp::Store && inst.args.size() == 2 && inst.args[0] == slot)
                    assigned = true;
            if (in[b] != next_in || out[b] != assigned) {
                in[b] = next_in;
                out[b] = assigned;
                changed = true;
            }
        }
    } while (changed);
    for (BlockId b = 0; b < fn.blocks.size(); ++b) {
        bool assigned = in[b];
        for (const auto& inst : fn.blocks[b].instructions) {
            if (inst.op == IrOp::Load && inst.args.size() == 1 && inst.args[0] == slot && !assigned)
                return false;
            if (inst.op == IrOp::Store && inst.args.size() == 2 && inst.args[0] == slot)
                assigned = true;
        }
    }
    return true;
}

/** Promote all nonescaping, definitely initialized 4-byte scalar allocas via iterated dominance
 * frontiers. / 经迭代支配边界提升所有不逃逸且必定初始化的 4 字节标量分配。 */
void
mem2reg(FunctionIR& fn) {
    const auto succ = successors(fn);
    const auto pred = predecessors(succ);
    const auto dom = dominators(fn, pred);
    std::vector<ValueId> slots;
    for (const auto& inst : fn.blocks[fn.entry].instructions)
        if (inst.op == IrOp::Alloca
            && inst.dst != no_value
            && inst.imm == 4
            && promotable(fn, inst.dst, pred))
            slots.push_back(inst.dst);
    std::vector<ValueId> replacement(fn.value_types.size(), no_value);
    for (const auto slot : slots) {
        std::vector<bool> defs(fn.blocks.size()), has_phi(fn.blocks.size());
        IrType type = IrType::Void;
        for (BlockId b = 0; b < fn.blocks.size(); ++b)
            for (const auto& inst : fn.blocks[b].instructions) {
                if (inst.op == IrOp::Store && inst.args.size() == 2 && inst.args[0] == slot) {
                    defs[b] = true;
                    const auto value_type = fn.value_types[inst.args[1]];
                    if (type == IrType::Void)
                        type = value_type;
                    if (type != value_type)
                        type = IrType::Ptr; // invalid mixed-type slot: leave to verifier
                }
                if (inst.op == IrOp::Load && inst.args.size() == 1 && inst.args[0] == slot) {
                    if (type == IrType::Void)
                        type = inst.type;
                    if (type != inst.type)
                        type = IrType::Ptr;
                }
            }
        if (type == IrType::Ptr || type == IrType::Void) {
            if (type == IrType::Void)
                std::erase_if(fn.blocks[fn.entry].instructions, [&](const Instruction& i) {
                    return i.op == IrOp::Alloca && i.dst == slot;
                });
            continue;
        }
        std::vector<BlockId> work;
        for (BlockId b = 0; b < defs.size(); ++b)
            if (defs[b])
                work.push_back(b);
        std::vector<ValueId> phi(fn.blocks.size(), no_value);
        while (!work.empty()) {
            const auto b = work.back();
            work.pop_back();
            for (const auto y : dom.frontier[b])
                if (!has_phi[y]) {
                    has_phi[y] = true;
                    Instruction inst;
                    inst.op = IrOp::Phi;
                    inst.type = type;
                    inst.dst = fn.new_value(type);
                    phi[y] = inst.dst;
                    fn.blocks[y].instructions.insert(
                        fn.blocks[y].instructions.begin(),
                        std::move(inst)
                    );
                    if (!defs[y])
                        work.push_back(y);
                }
        }
        replacement.resize(fn.value_types.size(), no_value);
        std::function<void(BlockId, std::vector<ValueId>)> rename =
            [&](BlockId b, std::vector<ValueId> stack) {
                if (phi[b] != no_value)
                    stack.push_back(phi[b]);
                auto& code = fn.blocks[b].instructions;
                for (auto& inst : code) {
                    if (inst.op == IrOp::Load && inst.args.size() == 1 && inst.args[0] == slot) {
                        if (!stack.empty())
                            replacement[inst.dst] = stack.back();
                        inst.op = IrOp::Alloca;
                        inst.type = IrType::Void;
                        inst.dst = no_value;
                        inst.args.clear();
                    } else if (
                        inst.op == IrOp::Store && inst.args.size() == 2 && inst.args[0] == slot
                    ) {
                        stack.push_back(inst.args[1]);
                        inst.op = IrOp::Alloca;
                        inst.args.clear();
                    } else if (inst.op == IrOp::Alloca && inst.dst == slot) {
                        inst.type = IrType::Void;
                        inst.dst = no_value;
                        inst.imm = 0;
                    }
                }
                for (const auto s : succ[b])
                    if (phi[s] != no_value && !stack.empty())
                        for (auto& inst : fn.blocks[s].instructions)
                            if (inst.op == IrOp::Phi && inst.dst == phi[s]) {
                                inst.incoming.emplace_back(b, stack.back());
                                break;
                            }
                for (const auto child : dom.children[b])
                    rename(child, stack);
            };
        rename(fn.entry, {});
        for (auto& block : fn.blocks)
            std::erase_if(block.instructions, [](const Instruction& i) {
                return i.op == IrOp::Alloca && i.dst == no_value && i.type == IrType::Void;
            });
        rewrite_uses(fn, replacement);
    }
}

/** Constant lattice: Unknown < Constant < Overdefined. / 常量格：未知、确定常量、非单一常量。 */
struct Lattice {
    enum class Kind { Unknown, Constant, Overdefined } kind = Kind::Unknown;
    std::uint32_t bits = 0;
    friend bool operator==(const Lattice&, const Lattice&) = default;
};

Lattice
merge(Lattice a, Lattice b) {
    if (a.kind == Lattice::Kind::Unknown)
        return b;
    if (b.kind == Lattice::Kind::Unknown)
        return a;
    if (a.kind == Lattice::Kind::Overdefined
        || b.kind == Lattice::Kind::Overdefined
        || a.bits != b.bits)
        return {Lattice::Kind::Overdefined, 0};
    return a;
}

/** Fold only operations whose bit-level SysY meaning is independent of host floating rounding mode.
 * / 仅折叠位级语义不依赖宿主浮点舍入模式的操作。 */
Lattice
evaluate(
    const Instruction& inst,
    const FunctionIR& fn,
    const std::vector<Lattice>& values,
    const std::set<std::pair<BlockId, BlockId>>& edges,
    BlockId block
) {
    using K = Lattice::Kind;
    if (inst.op == IrOp::ConstI32 || inst.op == IrOp::ConstF32)
        return {K::Constant, static_cast<std::uint32_t>(inst.imm)};
    if (inst.op == IrOp::Phi) {
        Lattice result;
        for (const auto& [pred, value] : inst.incoming)
            if (edges.contains({pred, block}))
                result = merge(result, values[value]);
        return result;
    }
    if (inst.op == IrOp::Param
        || inst.op == IrOp::Load
        || inst.op == IrOp::Call
        || inst.op == IrOp::Alloca
        || inst.op == IrOp::GlobalAddr
        || inst.op == IrOp::PtrAdd)
        return {K::Overdefined, 0};
    if (inst.args.empty())
        return {K::Overdefined, 0};
    for (const auto arg : inst.args)
        if (values[arg].kind == K::Overdefined)
            return {K::Overdefined, 0};
    for (const auto arg : inst.args)
        if (values[arg].kind == K::Unknown)
            return {};
    const auto a = values[inst.args[0]].bits;
    const auto b = inst.args.size() >= 2 ? values[inst.args[1]].bits : 0;
    const auto input = fn.value_types[inst.args[0]];
    if (inst.op == IrOp::Cmp) {
        bool result = false;
        if (input == IrType::F32) {
            const float x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
            switch (static_cast<CmpPred>(inst.imm)) {
            case CmpPred::Eq:
                result = x == y;
                break;
            case CmpPred::Ne:
                result = x != y;
                break;
            case CmpPred::Lt:
                result = x < y;
                break;
            case CmpPred::Le:
                result = x <= y;
                break;
            case CmpPred::Gt:
                result = x > y;
                break;
            case CmpPred::Ge:
                result = x >= y;
                break;
            }
        } else if (input == IrType::I32) {
            const auto x = static_cast<std::int32_t>(a), y = static_cast<std::int32_t>(b);
            switch (static_cast<CmpPred>(inst.imm)) {
            case CmpPred::Eq:
                result = x == y;
                break;
            case CmpPred::Ne:
                result = x != y;
                break;
            case CmpPred::Lt:
                result = x < y;
                break;
            case CmpPred::Le:
                result = x <= y;
                break;
            case CmpPred::Gt:
                result = x > y;
                break;
            case CmpPred::Ge:
                result = x >= y;
                break;
            }
        } else
            return {K::Overdefined, 0};
        return {K::Constant, static_cast<std::uint32_t>(result)};
    }
    if (input != IrType::I32)
        return {K::Overdefined, 0};
    switch (inst.op) {
    case IrOp::Add:
        return {K::Constant, a + b};
    case IrOp::Sub:
        return {K::Constant, a - b};
    case IrOp::Mul:
        return {K::Constant, a * b};
    case IrOp::Div:
    case IrOp::Rem: {
        const auto x = static_cast<std::int32_t>(a), y = static_cast<std::int32_t>(b);
        if (y == 0 || (x == std::numeric_limits<std::int32_t>::min() && y == -1))
            return {K::Overdefined, 0};
        return {K::Constant, static_cast<std::uint32_t>(inst.op == IrOp::Div ? x / y : x % y)};
    }
    default:
        return {K::Overdefined, 0};
    }
}

/** Propagate constants over executable edges, then prune known branches. /
 * 沿可执行边传播常量，并剪除条件已知的分支。 */
void
sccp(FunctionIR& fn) {
    using K = Lattice::Kind;
    auto succ = successors(fn);
    std::vector<bool> live(fn.blocks.size());
    std::set<std::pair<BlockId, BlockId>> edges;
    std::vector<Lattice> values(fn.value_types.size());
    live[fn.entry] = true;
    bool changed;
    do {
        changed = false;
        for (BlockId b = 0; b < fn.blocks.size(); ++b)
            if (live[b]) {
                for (const auto& inst : fn.blocks[b].instructions)
                    if (inst.dst != no_value) {
                        const auto next =
                            merge(values[inst.dst], evaluate(inst, fn, values, edges, b));
                        if (next != values[inst.dst]) {
                            values[inst.dst] = next;
                            changed = true;
                        }
                    }
                const auto& term = fn.blocks[b].terminator;
                auto mark = [&](BlockId dest) {
                    if (edges.emplace(b, dest).second)
                        changed = true;
                    if (!live[dest]) {
                        live[dest] = true;
                        changed = true;
                    }
                };
                if (term.kind == TermKind::Jump)
                    mark(term.yes);
                if (term.kind == TermKind::Branch) {
                    const auto cond = values[term.value];
                    if (cond.kind == K::Constant)
                        mark(cond.bits ? term.yes : term.no);
                    else if (cond.kind == K::Overdefined) {
                        mark(term.yes);
                        mark(term.no);
                    }
                }
            }
    } while (changed);
    // Unknown cyclic values are conservatively treated as nonconstant before CFG pruning.
    for (auto& block : fn.blocks) {
        auto& term = block.terminator;
        if (term.kind == TermKind::Branch && values[term.value].kind == K::Constant) {
            term.yes = values[term.value].bits ? term.yes : term.no;
            term.no = no_block;
            term.value = no_value;
            term.kind = TermKind::Jump;
        }
        for (auto& inst : block.instructions)
            if (inst.dst != no_value
                && inst.op != IrOp::Phi
                && inst.op != IrOp::ConstI32
                && inst.op != IrOp::ConstF32
                && inst.op != IrOp::Load
                && inst.op != IrOp::Call
                && values[inst.dst].kind == K::Constant
                && (inst.type == IrType::I32 || inst.type == IrType::F32)) {
                inst.op = inst.type == IrType::I32 ? IrOp::ConstI32 : IrOp::ConstF32;
                inst.imm = values[inst.dst].bits;
                inst.args.clear();
                inst.incoming.clear();
            }
    }
    // A folded branch can remove one edge while both endpoints remain reachable.
    // Phi incoming lists describe current edges, not historical reachability.
    const auto pred = predecessors(successors(fn));
    for (BlockId b = 0; b < fn.blocks.size(); ++b)
        for (auto& inst : fn.blocks[b].instructions)
            if (inst.op == IrOp::Phi)
                std::erase_if(inst.incoming, [&](const auto& edge) {
                    return std::ranges::find(pred[b], edge.first) == pred[b].end();
                });
    remove_unreachable(fn);
}

/** Retain side effects and their transitive dependencies. / 保留副作用及其传递依赖。 */
void
dce(FunctionIR& fn) {
    std::vector<bool> live(fn.value_types.size());
    for (const auto& block : fn.blocks) {
        if (block.terminator.value != no_value)
            live[block.terminator.value] = true;
        for (const auto& inst : block.instructions)
            if (inst.op == IrOp::Store
                || inst.op == IrOp::Call
                || inst.op == IrOp::Div
                || inst.op == IrOp::Rem) {
                for (const auto arg : inst.args)
                    live[arg] = true;
            }
    }
    bool changed;
    do {
        changed = false;
        for (const auto& block : fn.blocks)
            for (const auto& inst : block.instructions)
                if (inst.dst != no_value && live[inst.dst]) {
                    for (const auto arg : inst.args)
                        if (!live[arg]) {
                            live[arg] = true;
                            changed = true;
                        }
                    for (const auto& edge : inst.incoming)
                        if (!live[edge.second]) {
                            live[edge.second] = true;
                            changed = true;
                        }
                }
    } while (changed);
    for (auto& block : fn.blocks)
        std::erase_if(block.instructions, [&](const Instruction& inst) {
            return inst.dst != no_value
                   && !live[inst.dst]
                   && inst.op != IrOp::Call
                   && inst.op != IrOp::Div
                   && inst.op != IrOp::Rem;
        });
}

/** Pure expression key; exact operands avoid unsound algebraic rewrites for IEEE-754. /
 * 纯表达式键；精确操作数避免不安全的 IEEE-754 代数重写。 */
struct ExprKey {
    IrOp op;
    IrType type;
    std::vector<ValueId> args;
    std::int64_t imm;
    friend bool operator==(const ExprKey&, const ExprKey&) = default;
};

struct ExprHash {
    std::size_t
    operator()(const ExprKey& key) const {
        std::size_t h = static_cast<std::size_t>(key.op) * 131 + static_cast<std::size_t>(key.type);
        for (auto arg : key.args)
            h = (h ^ arg) * 1099511628211ULL;
        return (h ^ static_cast<std::uint64_t>(key.imm)) * 1099511628211ULL;
    }
};

/** Dominance-scoped whole-function CSE; never merges memory, calls, or possibly trapping
 * arithmetic. / 支配域内跨全函数 CSE；绝不合并内存、调用或可能陷入的算术。 */
void
gvn(FunctionIR& fn) {
    const auto dom = dominators(fn, predecessors(successors(fn)));
    std::vector<ValueId> replacement(fn.value_types.size(), no_value);
    using Table = std::unordered_map<ExprKey, ValueId, ExprHash>;
    std::function<void(BlockId, Table)> visit = [&](BlockId b, Table table) {
        for (auto& inst : fn.blocks[b].instructions) {
            for (auto& arg : inst.args)
                arg = resolve(arg, replacement);
            bool eligible = inst.dst != no_value;
            switch (inst.op) {
            case IrOp::ConstI32:
            case IrOp::ConstF32:
            case IrOp::Add:
            case IrOp::Sub:
            case IrOp::Mul:
            case IrOp::Cmp:
            case IrOp::IToF:
            case IrOp::FToI:
            case IrOp::PtrAdd:
                break;
            default:
                eligible = false;
                break;
            }
            if (!eligible)
                continue;
            ExprKey key{inst.op, inst.type, inst.args, inst.imm};
            if (const auto it = table.find(key); it != table.end())
                replacement[inst.dst] = it->second;
            else
                table.emplace(std::move(key), inst.dst);
        }
        for (const auto child : dom.children[b])
            visit(child, table);
    };
    visit(fn.entry, {});
    rewrite_uses(fn, replacement);
    for (auto& block : fn.blocks)
        std::erase_if(block.instructions, [&](const Instruction& inst) {
            return inst.dst != no_value && replacement[inst.dst] != no_value;
        });
}

} // namespace

std::vector<std::string>
verify_ir(const ModuleIR& module) {
    std::vector<std::string> errors;
    for (const auto& fn : module.functions) {
        const auto initial_errors = errors.size();
        auto fail = [&](BlockId b, const std::string& reason) {
            errors.push_back("IR " + fn.name + ": block " + std::to_string(b) + ": " + reason);
        };
        if (fn.entry >= fn.blocks.size()) {
            errors.push_back("IR " + fn.name + ": invalid entry block");
            continue;
        }
        const auto succ = successors(fn);
        const auto pred = predecessors(succ);
        std::vector<BlockId> def_block(fn.value_types.size(), no_block);
        std::vector<std::size_t> def_pos(fn.value_types.size());
        for (BlockId b = 0; b < fn.blocks.size(); ++b) {
            const auto& block = fn.blocks[b];
            const auto& term = block.terminator;
            if (term.kind == TermKind::None)
                fail(b, "missing terminator");
            if ((term.kind == TermKind::Jump || term.kind == TermKind::Branch)
                && term.yes >= fn.blocks.size())
                fail(b, "invalid target");
            if (term.kind == TermKind::Branch && term.no >= fn.blocks.size())
                fail(b, "invalid false target");
            if (term.kind == TermKind::Branch
                && (term.value >= fn.value_types.size()
                    || fn.value_types[term.value] != IrType::I32))
                fail(b, "branch condition must be i32");
            if (term.kind == TermKind::Return
                && ((term.value == no_value) != (fn.return_type == IrType::Void)
                    || (term.value != no_value
                        && (term.value >= fn.value_types.size()
                            || fn.value_types[term.value] != fn.return_type))))
                fail(b, "return type mismatch");
            bool ended_phi = false;
            for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
                const auto& inst = block.instructions[pos];
                if (inst.op != IrOp::Phi)
                    ended_phi = true;
                else if (ended_phi)
                    fail(b, "Phi after non-Phi instruction");
                if (inst.dst != no_value) {
                    if (inst.dst >= fn.value_types.size())
                        fail(b, "invalid result ID");
                    else {
                        if (def_block[inst.dst] != no_block)
                            fail(b, "duplicate SSA definition");
                        def_block[inst.dst] = b;
                        def_pos[inst.dst] = pos;
                        if (fn.value_types[inst.dst] != inst.type)
                            fail(b, "result type table mismatch");
                    }
                } else if (inst.type != IrType::Void)
                    fail(b, "non-void instruction missing result");
                for (auto arg : inst.args)
                    if (arg >= fn.value_types.size())
                        fail(b, "invalid operand ID");
                if (std::ranges::any_of(inst.args, [&](ValueId arg) {
                        return arg >= fn.value_types.size();
                    }))
                    continue;
                const auto arity = [&](std::size_t expected) {
                    if (inst.args.size() != expected)
                        fail(b, "incorrect operand count");
                    return inst.args.size() == expected;
                };
                const auto ty = [&](std::size_t index) { return fn.value_types[inst.args[index]]; };
                switch (inst.op) {
                case IrOp::ConstI32:
                    if (!arity(0) || inst.type != IrType::I32)
                        fail(b, "invalid i32 constant");
                    break;
                case IrOp::ConstF32:
                    if (!arity(0) || inst.type != IrType::F32)
                        fail(b, "invalid f32 constant");
                    break;
                case IrOp::Param:
                    if (!arity(0)
                        || inst.imm < 0
                        || static_cast<std::size_t>(inst.imm) >= fn.parameters.size()
                        || (inst.imm >= 0
                            && static_cast<std::size_t>(inst.imm) < fn.parameters.size()
                            && inst.type != fn.parameters[static_cast<std::size_t>(inst.imm)]))
                        fail(b, "invalid parameter");
                    break;
                case IrOp::Alloca:
                    if (!arity(0) || inst.type != IrType::Ptr || inst.imm <= 0)
                        fail(b, "invalid alloca");
                    break;
                case IrOp::GlobalAddr:
                    if (!arity(0) || inst.type != IrType::Ptr || inst.symbol.empty())
                        fail(b, "invalid global address");
                    break;
                case IrOp::PtrAdd:
                    if (arity(2)
                        && (inst.type != IrType::Ptr
                            || ty(0) != IrType::Ptr
                            || ty(1) != IrType::I32))
                        fail(b, "invalid pointer offset");
                    break;
                case IrOp::Load:
                    if (arity(1)
                        && (ty(0) != IrType::Ptr
                            || (inst.type != IrType::I32 && inst.type != IrType::F32)))
                        fail(b, "invalid load type");
                    break;
                case IrOp::Store:
                    if (arity(2)
                        && (inst.type != IrType::Void
                            || ty(0) != IrType::Ptr
                            || (ty(1) != IrType::I32 && ty(1) != IrType::F32))) {
                        fail(b, "invalid store type");
                    }
                    break;
                case IrOp::Add:
                case IrOp::Sub:
                case IrOp::Mul:
                case IrOp::Div:
                case IrOp::Rem:
                    if (arity(2)
                        && (ty(0) != ty(1)
                            || inst.type != ty(0)
                            || (inst.type != IrType::I32 && inst.type != IrType::F32)
                            || (inst.op == IrOp::Rem && inst.type != IrType::I32)))
                        fail(b, "invalid arithmetic type");
                    break;
                case IrOp::Cmp:
                    if (arity(2)
                        && (inst.type != IrType::I32
                            || ty(0) != ty(1)
                            || (ty(0) != IrType::I32 && ty(0) != IrType::F32)
                            || inst.imm < static_cast<std::int64_t>(CmpPred::Eq)
                            || inst.imm > static_cast<std::int64_t>(CmpPred::Ge)))
                        fail(b, "invalid comparison");
                    break;
                case IrOp::IToF:
                    if (arity(1) && (inst.type != IrType::F32 || ty(0) != IrType::I32))
                        fail(b, "invalid int-to-float cast");
                    break;
                case IrOp::FToI:
                    if (arity(1) && (inst.type != IrType::I32 || ty(0) != IrType::F32))
                        fail(b, "invalid float-to-int cast");
                    break;
                case IrOp::Call:
                    if (inst.symbol.empty())
                        fail(b, "call without symbol");
                    break;
                case IrOp::Phi:
                    if (!inst.args.empty())
                        fail(b, "Phi has ordinary operands");
                    break;
                }
                if (inst.op == IrOp::Phi) {
                    std::set<BlockId> got;
                    for (const auto& [p, value] : inst.incoming) {
                        if (p >= fn.blocks.size() || value >= fn.value_types.size())
                            fail(b, "invalid Phi incoming");
                        else {
                            if (!got.insert(p).second)
                                fail(b, "duplicate Phi predecessor");
                            if (fn.value_types[value] != inst.type)
                                fail(b, "Phi value type mismatch");
                        }
                    }
                    if (got != std::set<BlockId>(pred[b].begin(), pred[b].end()))
                        fail(b, "Phi predecessor set mismatch");
                }
            }
        }
        if (errors.size() != initial_errors)
            continue;
        const auto live = reachable(fn, succ);
        // 中文：支配检查只针对可达路径；不可达块将在优化入口被删除。
        // English: Dominance matters on executable paths; cleanup removes dead blocks first.
        const auto dom = dominators(fn, pred);
        for (BlockId b = 0; b < fn.blocks.size(); ++b)
            if (live[b]) {
                const auto& block = fn.blocks[b];
                for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
                    const auto& inst = block.instructions[pos];
                    for (auto arg : inst.args) {
                        if (def_block[arg] == no_block
                            || (def_block[arg] == b && def_pos[arg] >= pos))
                            fail(b, "use before definition");
                        else if (!dom.dom[b][def_block[arg]])
                            fail(b, "operand definition does not dominate use");
                    }
                    if (inst.op == IrOp::Phi)
                        for (const auto& [p, arg] : inst.incoming)
                            if (live[p]
                                && (def_block[arg] == no_block || !dom.dom[p][def_block[arg]]))
                                fail(b, "Phi input definition does not dominate predecessor edge");
                }
                if (block.terminator.value != no_value) {
                    const auto arg = block.terminator.value;
                    if (arg >= def_block.size() || def_block[arg] == no_block)
                        fail(b, "undefined terminator operand");
                    else if (!dom.dom[b][def_block[arg]])
                        fail(b, "terminator operand definition does not dominate use");
                }
            }
    }
    return errors;
}

std::vector<std::string>
optimize(ModuleIR& module, int level, std::vector<PassStats>* stats) {
    if (auto errors = verify_ir(module); !errors.empty())
        return errors;
    if (level <= 0)
        return {};
    for (auto& fn : module.functions) {
        const auto count = [&] {
            std::size_t n = 0;
            for (const auto& block : fn.blocks)
                n += block.instructions.size();
            return n;
        };
        const auto run = [&](std::string_view name, auto&& pass) {
            if (!stats) {
                pass();
                return;
            }
            PassStats item;
            item.function = fn.name;
            item.pass = name;
            item.blocks_before = fn.blocks.size();
            item.instructions_before = count();
            const auto start = std::chrono::steady_clock::now();
            pass();
            item.duration_ns =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - start
                )
                                               .count());
            item.blocks_after = fn.blocks.size();
            item.instructions_after = count();
            stats->push_back(std::move(item));
        };
        run("unreachable", [&] { remove_unreachable(fn); });
        run("mem2reg", [&] { mem2reg(fn); });
        run("sccp", [&] { sccp(fn); });
        run("dce", [&] { dce(fn); });
        if (level >= 2) {
            run("gvn", [&] { gvn(fn); });
            run("dce-after-gvn", [&] { dce(fn); });
        }
    }
    return verify_ir(module);
}

} // namespace sysy
