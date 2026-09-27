# SysY 运行时库

本目录由原 `lib.tar.gz` 解包得到，包含运行时源码、头文件和面向不同目标架构的预编译库。

| 文件 | 用途 |
| --- | --- |
| `sylib.c` | SysY 运行时库实现 |
| `sylib.h` | C 接口声明 |
| `sylib.so` | x86 平台动态库，可供 LLVM JIT 等场景使用 |
| `libsysy_x86.a` | x86 平台静态库 |
| `libsysy_riscv.a` | RISC-V 平台静态库 |
| `libsysy_aarch.a` | AArch64/ARM 平台静态库 |

## 静态链接示例

### x86

```sh
gcc main.o -L./lib -lsysy_x86 -o main
```

### RISC-V

```sh
riscv64-linux-gnu-gcc main.o -L./lib -lsysy_riscv -static -o main
```

### AArch64

```sh
aarch64-linux-gnu-gcc main.o -L./lib -lsysy_aarch -static -o main
```

运行时接口的完整说明见 [`../docs/SysY2022运行时库-V1.md`](../docs/SysY2022运行时库-V1.md)。
