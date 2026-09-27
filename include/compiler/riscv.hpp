#pragma once

#include "compiler/ir.hpp"

#include <expected>
#include <iosfwd>
#include <string>

namespace sysy {

/**
 * Emit GNU assembler-compatible RV64GC LP64D assembly without writing a partial
 * artifact on failure. The caller owns output publication. / 输出 GNU 汇编兼容的
 * RV64GC LP64D 汇编；失败时不写入半成品，输出发布由调用方负责。
 */
[[nodiscard]] std::expected<void, std::string> emit_rv64(const ModuleIR& module, std::ostream& out);

} // namespace sysy
