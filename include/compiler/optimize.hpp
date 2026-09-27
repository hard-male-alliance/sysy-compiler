#pragma once

#include "compiler/ir.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sysy {

/**
 * Per-function pass measurement. Counts include Phi nodes but exclude terminators.
 * Timing is collected only when requested to protect tiny-input startup latency.
 * / 单函数单遍优化测量；数量包含 Phi，但不含终结符。仅在请求时计时，以保护短输入启动性能。
 */
struct PassStats {
    std::string function;
    std::string pass;
    std::size_t blocks_before = 0;
    std::size_t blocks_after = 0;
    std::size_t instructions_before = 0;
    std::size_t instructions_after = 0;
    std::uint64_t duration_ns = 0;
};

/**
 * Verify and optimize a module without changing its externally visible behavior.
 * Level 0 verifies only. Level 1 promotes nonescaping scalar stack slots,
 * retaining typed Undef for indeterminate paths, and runs CFG/SCCP/DCE cleanup.
 * Level 2 also eliminates dominance-scoped scalar common subexpressions. Errors
 * are internal IR diagnostics and must prevent code emission. / 验证并优化模块，保持
 * 可观察行为。0 级只验证；1 级提升不逃逸的标量栈槽，用带类型 Undef 保留未定路径，
 * 并清理 CFG、执行 SCCP 和 DCE；2 级进一步在支配域内消除标量公共子表达式。
 * 返回的错误是内部 IR 诊断，必须阻止代码生成。
 *
 * Example / 示例：`if (auto errors = optimize(module, 2); !errors.empty()) ...;`
 */
[[nodiscard]] std::vector<std::string>
optimize(ModuleIR& module, int level, std::vector<PassStats>* stats = nullptr);

} // namespace sysy
