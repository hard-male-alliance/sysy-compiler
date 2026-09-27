# Compiler-core integration review (2026-09-27)

## Scope and evidence

Reviewed the repository-local SysY contract and architecture notes, then inspected the lexer, parser, semantic model, IR lowering, optimizer/verifier, RV64 emitter, and driver. The emphasis was executable correctness: name resolution, initializer layout, short-circuit control flow, SSA/Phi handling, integer/float conversions, LP64D argument placement, and output publication. This is not a proof of full language conformance.

Target execution evidence: `tests/sysy_conformance.py --execute on` passed with the WSL-hosted compiler, RISC-V GNU toolchain, and QEMU (0 failed assertions). A separate optimized run in `.temp/review_core/check_opt.py` compiled, linked, and executed all 17 current valid conformance fixtures at each of `-O1` and `-O2`, comparing process status and stdout with the fixture expectations. All 34 optimized executions passed, including short-circuit, loop control, arrays, float, and mixed 20-argument ABI cases. The script is a review probe, not a substitute for wider adversarial testing.

## Finding 1 — resolved during integration: a local object hid a callable function

**Priority:** P1 correctness before fix; **status:** resolved in the shared tree. **Confidence:** high for the demonstrated rejection; interpretation of call-name namespace is supported by the local SysY specification.

Original `src/semantic.cpp::call()` used `find(node.text)`, which searched innermost lexical object scopes first. Thus a local object named like a function caused a syntactically explicit call to be rejected as `undefined function 'f'`. Reproducer in `.temp/review_core/shadow_call.sy`:

```c
int f(){return 7;}
int main(){int f=1; return f()+f;}
```

The pre-fix compiler reported `undefined function 'f'` at the call. `docs/task/SysY2022语言定义-V1.md`, line 100, explicitly permits variables to share names with functions, while the `Ident '(' ... ')'` grammar discriminates calls from object references. The correction was to resolve calls through the function namespace independently of local objects. The semantic owner implemented `find_function()` and switched `call()` to it (`src/semantic.cpp`, currently lines 67–72 and 218–220). After rebuilding the Windows Debug compiler, the same reproducer returned exit code 0 and emitted an 808-byte assembly file.

## Remaining risk and suggested next gate

No additional concrete defect was established in the reviewed paths. The current regression corpus is small relative to the language's combinatorial space. In particular, executable `-O0/-O1/-O2` differential cases with nested Phi cycles, runtime-dependent mixed-brace initializers, recursive array arguments, and float exceptional values would raise confidence. These are *coverage gaps*, not asserted bugs. The existing test matrix exercises ordinary Phi/loop behavior and mixed register/stack ABI cases, so that evidence should not be discounted.
