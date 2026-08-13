# MC-001 改造前基线

记录日期：2026-08-13

## 源码与分支

- 基线源码提交：`ffd46559861e08a1f16f6bb9c6b980b709906376`（`feat(croutine): 协程新增 processor_id 字段`）
- 记录分支：`development`，从上述 `master` 提交创建。
- 工作树在记录前已存在未提交的 `MINICYBER_ROADMAP.md` 修改和 `docs/refactor/` 文档；本任务未改动产品代码。

## 环境与依赖

- OS：Ubuntu 22.04 系列，Linux `6.8.0-124-generic`，x86_64。
- CMake：3.22.1；编译器：g++ 11.4.0；Protobuf：libprotoc 3.12.4。
- GoogleTest：`release-1.12.1`（`58d77fa8070e8cec2dc1ed015d66b454c8d78850`）。项目声明的镜像 `https://kkgithub.com/google/googletest.git` 不可访问，因此为本次基线将从 `https://github.com/google/googletest.git` 获得的源码放在 `/tmp/minicyber-googletest2`，以 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` 覆盖；未修改项目 CMake 文件。
- 为规避此前 ccache 临时目录权限问题，配置时清除了编译器 launcher 和 `CCACHE_DIR`。

## 可复现命令

```bash
git switch development
git clone --depth 1 --branch release-1.12.1 \
  https://github.com/google/googletest.git /tmp/minicyber-googletest2
env -u CCACHE_DIR -u CMAKE_CXX_COMPILER_LAUNCHER -u CMAKE_C_COMPILER_LAUNCHER \
  cmake -S . -B build-baseline -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER_LAUNCHER= -DCMAKE_C_COMPILER_LAUNCHER= \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/tmp/minicyber-googletest2 \
  -DFETCHCONTENT_QUIET=OFF
cmake --build build-baseline -j2
ctest --test-dir build-baseline --output-on-failure
```

## 结果

- Debug 配置成功，构建目录为 `build-baseline`。
- 构建失败（退出码 2）。`minicyber_core` 编译 `src/swap.S` 时，CMake 为汇编器生成 `ASM_DEFINES = --defsym minicyber_core_EXPORTS`，但 GNU as 要求 `--defsym name=value`，报错：`Fatal error: bad defsym; format is --defsym name=value`。
- 构建在该错误前已经成功生成 `minicyber_proto`、`gtest`、`gtest_main`、`gmock` 和 `gmock_main`；核心库与测试可执行文件未生成。
- CTest 已注册 47 个测试；执行 `ctest --output-on-failure` 后为 0 通过、47 `Not Run`，原因均为对应测试可执行文件尚未生成，而不是测试断言失败。

## 已知失败与边界

- `https://kkgithub.com/google/googletest.git` 返回“repository not found”或 TLS 握手中断；直连 GitHub 可用。后续构建若不提供 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`，将再次受该镜像影响。
- 汇编器定义传递问题是当前未改造源码的构建失败，应在后续有明确任务覆盖构建系统或汇编上下文时单独修复；MC-001 仅记录，不改动产品实现。
