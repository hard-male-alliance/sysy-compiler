# SysY 运行时库

本目录来自课程提供的 `lib.tar.gz`，保留原始源码、头文件与预编译库。
**预编译归档不能仅凭目标架构名称选择**：还必须匹配目标操作系统、C 运行库与 ABI。

| 文件 | 用途 |
| --- | --- |
| `sylib.c`, `sylib.h` | SysY 运行时实现及接口；构建 GNU/Linux 目标运行时的源文件 |
| `sylib.so`, `libsysy_x86.a` | 课程提供的 x86 预编译库 |
| `libsysy_riscv.a` | 课程提供的 RISC-V 归档，依赖 Newlib，**不要与 glibc GNU/Linux 工具链混链** |
| `libsysy_aarch.a` | 课程提供的 AArch64/ARM 归档；使用前仍须检查其目标 ABI |

## GNU/Linux RISC-V 静态链接

在仓库根目录执行，需安装 `riscv64-linux-gnu-gcc`、`riscv64-linux-gnu-ar`，
以及用于运行目标程序的 `qemu-riscv64`。CMake 的此选项只增加一个目标运行时归档，
宿主 `compiler` 仍是单一可执行文件；Windows/macOS 的默认构建不需要交叉工具链。

```sh
cmake -S . -B .cache/build/wsl-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSYSY_BUILD_RISCV_RUNTIME=ON
cmake --build .cache/build/wsl-runtime --parallel
mkdir -p .temp
.cache/build/wsl-runtime/compiler tests/conformance/basic.sy -o .temp/basic.s
riscv64-linux-gnu-gcc -static -march=rv64gc -mabi=lp64d \
  .temp/basic.s .cache/build/wsl-runtime/runtime/riscv64-linux-gnu/libsysy.a \
  -o .temp/basic.elf
qemu-riscv64 .temp/basic.elf
```

配置时还可通过 `-DSYSY_RISCV_CC=/path/to/riscv64-linux-gnu-gcc`
和 `-DSYSY_RISCV_AR=/path/to/riscv64-linux-gnu-ar` 显式选用同一套工具链。
链接最终程序必须使用与归档构建时**相同的 GNU/Linux RISC-V 工具链及 ABI**；
若切换了工具链，请在独立构建目录重新配置／构建，不能复用旧归档。
`-fcommon` 仅用于编译原始运行时源码，以保留其头文件暂定定义的旧工具链语义；
不要把它当作修补 Newlib/glibc 不兼容的链接器选项，也不要伪造 `_impure_ptr`。

运行时接口见 [`../../docs/task/SysY2022运行时库-V1.md`](../../docs/task/SysY2022运行时库-V1.md)。
ABI 调查、验证命令和限制见 [`../../docs/architecture/runtime-linking.md`](../../docs/architecture/runtime-linking.md)。
