#pragma once

#include "compiler/ir.hpp"
#include "compiler/semantic.hpp"

namespace sysy {

/** Lowering result; reject module if errors is nonempty. / 降低结果；errors 非空时必须拒绝 module。 */
struct LowerResult {
    ModuleIR module;
    std::vector<FrontendError> errors;
    [[nodiscard]] bool ok() const { return errors.empty(); }
};

/** Convert a successfully analyzed immutable AST into typed control-flow IR. / 将已成功语义分析的不可变 AST 转换为类型化控制流 IR。 */
[[nodiscard]] LowerResult lower(const Program& program, const SemanticModel& model);

} // namespace sysy
