# 第一课：CMake 工程骨架 & 项目架构设计

> 对应阶段：Week 1-3 / Todo 1
> 关键词：CMake、FetchContent、编译单元、头文件分离

---

## 引言

这是我们项目的第一步。在写任何业务代码之前，我们需要先把"地基"打好——也就是整个项目的构建系统和目录结构。

我们要做的是一个低延迟撮合引擎，最终会有十几个源文件、测试套件、性能 benchmark，还要依赖 Google Test 这样的第三方库。如果一开始只用 `g++ main.cpp` 来编译，等文件一多就会乱成一团。所以第一件事就是引入 CMake——业界标准的 C++ 构建工具，让它帮我们管理文件依赖、第三方库、编译选项。

完成这一课后，我们有了一个可以直接 `cmake --build` 的完整工程骨架，后续每新增一个模块，只需要在 `CMakeLists.txt` 里加几行。接下来我们会开始实现第一个真正的业务模块：`Order` 结构体。

---

## 一、为什么要学 CMake？

### 现实中用在哪里

量化私募公司（幻方、九坤、明汯等）的 C++ 项目**全部使用 CMake 或 Bazel 构建**。  
你在面试中大概率会被问到：

- "你的项目怎么构建的？"
- "如果要加一个第三方库，你怎么处理依赖？"
- "你的 CI/CD 怎么跑测试？"

如果你只会 `g++ main.cpp -o main`，会直接暴露没有工程经验。

### CMake 解决的核心问题

| 问题 | 没有 CMake | 有 CMake |
|---|---|---|
| 多文件编译 | 手写 Makefile，维护成本高 | `add_executable` 自动管理 |
| 第三方依赖 | 手动下载、编译、链接 | `FetchContent` 自动拉取 |
| 跨平台 | 不同系统命令不同 | CMake 自动适配 |
| Debug/Release | 手写 `-O2 -DNDEBUG` | `CMAKE_BUILD_TYPE` 一键切换 |

---

## 二、本项目的 CMake 结构解析

### 根目录 `CMakeLists.txt` 的关键概念

```cmake
cmake_minimum_required(VERSION 3.20)
project(MatchingEngine VERSION 1.0)
```

**`project()` 做了什么**：设置项目名称、版本，并自动检测 C/C++ 编译器。

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

**为什么用 C++20**：`std::atomic`、`std::jthread`、Concepts 都是 C++20 特性。量化公司普遍已迁移到 C++17/20。

```cmake
include(FetchContent)
FetchContent_Declare(googletest ...)
FetchContent_MakeAvailable(googletest)
```

**FetchContent 是什么**：CMake 内置的依赖管理器。等价于 Python 的 `pip install`，但在构建时自动从 GitHub 下载并编译 GTest/GBenchmark。  
**生产中的替代品**：Conan、vcpkg，但 FetchContent 不需要额外安装工具，适合个人项目。

### target 的概念（最重要）

```cmake
add_library(matching_engine_lib STATIC ...)
add_executable(matching_engine src/main.cpp)
target_link_libraries(matching_engine PRIVATE matching_engine_lib)
```

- **`add_library`**：把核心代码编译成静态库（`.a` 文件），测试和 benchmark 都链接这个库
- **`PRIVATE` vs `PUBLIC`**：`PRIVATE` 表示这个依赖只在当前 target 内部使用，不传播给依赖它的 target——这影响头文件的可见性
- **为什么分离 library 和 executable**：这样 `tests/` 和 `benchmarks/` 都可以链接同一个库，不需要重复编译

---

## 三、头文件与实现文件分离（`.h` vs `.cpp`）

### 规则

| 放在 `.h` 中 | 放在 `.cpp` 中 |
|---|---|
| 类声明、函数声明 | 函数实现 |
| `inline` 函数 | 非 inline 的成员函数 |
| 模板类/函数的完整实现 | 静态变量定义 |

### 为什么要分离

- **编译速度**：修改 `.cpp` 只重新编译该文件；修改 `.h` 会导致所有包含它的文件重新编译
- **接口与实现解耦**：`.h` 是对外的"合同"，`.cpp` 是内部实现细节
- **模板类例外**：`SPSCRingBuffer` 是模板类，**整个实现必须放在 `.h` 里**，否则链接时找不到具体化版本

---

## 四、编译命令（你需要记住的）

```bash
# 首次配置（在项目根目录）
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# 编译
cmake --build . -j$(nproc)

# 运行测试
./tests/test_order_book
# 或用 ctest
ctest --output-on-failure

# 运行 benchmark
./benchmarks/bench_order_book --benchmark_format=json
```

**`-DCMAKE_BUILD_TYPE=Release`** 的意义：开启 `-O2` 优化，关闭 assert，**benchmark 测量必须用 Release 模式**，否则数据没有参考价值。

---

## 五、怎么验证这一课的成果

这一课的目标是搭出一个能跑的骨架，验证方式就是跑通整个构建流程，没有报错就算过关：

```bash
# 1. 首次配置（会自动下载 googletest 和 googlebenchmark，需要联网）
cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake -B build/release -DCMAKE_BUILD_TYPE=Release

# 2. 编译
cmake --build build/debug -j$(nproc)

# 3. 跑测试（目前项目里已有的测试用例）
cd build/debug && ctest --output-on-failure
```

如果 `ctest` 输出全是 `PASSED`，CMake 骨架就正确了。这一步出问题通常是：FetchContent 下载失败（检查网络），或者缺编译器（`sudo apt install build-essential`）。

---

## 六、面试中如何讲这部分

> "项目使用 CMake 构建，通过 FetchContent 集成 Google Test 和 Google Benchmark，核心逻辑编译为静态库，可执行文件和测试套件分别链接，支持 Debug/Release 双模式切换。"

这一句话就够了，显示你有工程素养。

---

## 七、下一步

学完本课后，你应该能回答：
- [ ] CMake 中 `target_link_libraries` 的 PRIVATE/PUBLIC/INTERFACE 区别是什么？
    - 三者用来表示依赖关系，表示库依赖的传递性
    - PRIVATE表示只有自己用
    - PUBLIC表示自己用，也传递给依赖它的库
    - INTERFACE表示自己不用，但传递给依赖它的库（常用于纯头文件库或接口库）

- [ ] 为什么模板类的实现必须放在头文件里？
    - 模板类是泛型编程，编译器无法提前知道具体类型，所以无法生成代码
    - 必须在头文件里写完整的实现，否则链接时找不到具体化版本
    - 模板类是在编译时实例化的，它不是函数，而是生成函数的规则

- [ ] `cmake --build . -j4` 中 `-j4` 是什么意思？（提示：并行编译）
    - `-j4` 表示同时编译 4 个文件，提高编译速度

**下一课预告**：`Order` 结构体设计——为什么用整数定价、为什么要 `alignas(64)`、什么是 cache line。
