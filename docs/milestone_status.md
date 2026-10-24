# M0 里程碑状态报告

状态：**M0 工程实现与构建测试已完成；G0 已迁移到正常 Git 工作目录。**

## 1. 实际项目路径

`/home/cy/Code/SgsServerRepo`

M0 最初位于 `/home/cy/Code/SgsServer`。该目录的 `.git` 是只读 tmpfs 挂载，无法初始化仓库；G0 按要求将源码复制到上述新目录。新目录是后续服务端正式开发目录，复制过程排除了原 `.git`、构建目录、参考压缩包、临时文件和编译产物。

## 2. 开发环境

| 项目 | 实际信息 |
| --- | --- |
| 操作系统 | Ubuntu 26.04.1 LTS (Resolute Raccoon)，Linux 7.0.0-38-generic，x86_64 |
| C++ 编译器 | g++ 15.2.0 (Ubuntu 15.2.0-16ubuntu1) |
| C++20 | 已由 CMake 以 `-std=c++20` 对实际工程编译验证 |
| CMake | 4.2.3 |
| Git | 2.53.0 |
| CTest | 4.2.3 |

基础工具均已存在，本阶段没有安装任何系统组件或第三方依赖。

## 3. 最终项目目录树

```text
/home/cy/Code/SgsServerRepo/
├── .git/                          # 正常可写的本地 Git 元数据
├── .gitignore
├── CMakeLists.txt
├── README.md
├── build/                         # 本地验收产物，已忽略
│   ├── cardgame-server
│   └── cardgame-smoke-test
├── docs/
│   ├── architecture.md
│   └── milestone_status.md
├── include/
│   └── cardgame/
│       └── server/
│           └── application.h
├── src/
│   ├── application.cpp
│   └── main.cpp
├── tests/
│   └── smoke_test.cpp
```

`build/` 不纳入版本控制；原目录中的 `.agents/`、`.codex/`、只读 `.git/` 和参考 ZIP 均未复制。

## 4. 新增或修改的文件

- `CMakeLists.txt`：配置 C++20、警告、核心库、服务端程序与 CTest。
- `.gitignore`：排除构建产物、编辑器文件、Agent 元数据和本地参考压缩包。
- `README.md`：说明项目、技术栈、M0 状态和构建运行方法。
- `include/cardgame/server/application.h`：声明最小 `Application` 入口。
- `src/application.cpp`：实现名称、版本及启动成功信息输出。
- `src/main.cpp`：仅构造并运行 `Application`。
- `tests/smoke_test.cpp`：无第三方测试框架的启动 smoke test。
- `docs/architecture.md`：记录当前及未来模块的职责边界。
- `docs/milestone_status.md`：本验收报告。

## 5. Application 启动流程

操作系统进入 `main` 后，`main` 以 `std::cout` 构造 `Application`，再调用 `run()`。`run()` 输出应用名称、版本和启动成功信息并返回 0，`main` 将该退出码直接返回给操作系统。输出流仍由调用方所有，`Application` 仅持有非拥有引用。

M0 启动期间不会创建 Socket、工作线程、Lua 虚拟机或原版扩展包对象。

## 6. CMake 构建结果

执行命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
```

结果：配置、生成和构建均成功。GNU 15.2.0 编译器完成 C++20 编译，生成 `build/cardgame-server` 和 `build/cardgame-smoke-test`，未出现编译警告或错误。

## 7. CTest 执行结果

执行命令：

```bash
ctest --test-dir build --output-on-failure
```

结果：

```text
Test #1: application_smoke ................ Passed
Test #2: server_starts .................... Passed
100% tests passed, 0 tests failed out of 2
```

第一项直接验证 `Application` 返回 0 且输出含名称和版本；第二项由 CTest 启动最终服务端程序，验证退出码和约定输出。

## 8. 实际程序输出

执行 `./build/cardgame-server`，退出码为 0，输出为：

```text
CardGameServer 0.1.0-dev
CardGameServer started successfully.
```

## 9. 已知问题与 Git 迁移

- 原目录的只读 `.git/` 问题已通过迁移到 `/home/cy/Code/SgsServerRepo` 解决；没有删除、覆盖或卸载原挂载点。新目录已使用 `main` 分支初始化，远程推送状态以 G0 最终验收结果为准。
- M0 没有网络及游戏业务能力，这是阶段范围约束，不是运行缺陷。

## 10. M1 建议（未开始）

总体架构负责人确认双端通信协议与生命周期约束后，M1 再引入 Boost.Asio，并只实现可独立测试的网络启动、连接与关闭基础设施。建议先明确连接所有权、停止顺序、错误传播和最小协议边界，再增加 Session；不要在网络基础尚未稳定时加入房间、Lua 或游戏规则。

本次开发到 M0 为止，不包含 M1 实现，也没有自行制定任何通信协议。
