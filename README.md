# CardGameServer

CardGameServer 是一个独立开发的 Linux 多人回合制卡牌游戏服务端。本仓库当前处于 M0，目标是提供干净、可构建、可测试的进程骨架；它不依赖 FreeKill，也不兼容或复用其客户端、服务端和扩展包。

## 技术栈

- C++20
- CMake 3.20 或更高版本
- CTest
- Linux / GCC（代码本身不写死发行版路径）

当前仅实现 `Application` 启动入口、版本与成功信息输出，以及最小 smoke test。尚未实现网络、协议、线程、房间、游戏规则、Lua、认证、数据库或资源包功能。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
```

Release 构建可将 `CMAKE_BUILD_TYPE` 改为 `Release`。

## 运行

```bash
./build/cardgame-server
```

## 测试

```bash
ctest --test-dir build --output-on-failure
```

## 后续阶段概览

M1 可在总体架构和双端通信协议确定后，引入实际需要的 Boost.Asio 网络基础设施，并明确连接生命周期与错误处理。后续里程碑再逐步加入会话、房间、权威游戏状态和 Lua 规则模块；每个阶段只增加当期可验证的能力。

架构边界见 [docs/architecture.md](docs/architecture.md)，当前验收状态见 [docs/milestone_status.md](docs/milestone_status.md)。
