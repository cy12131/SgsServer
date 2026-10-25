# SgsServer

SgsServer 是独立开发的 Linux 多人在线回合制卡牌游戏服务端。M2-S 在异步 SGS/1 网络层上实现了进程内临时用户、大厅、房间、座位和准备状态管理，不依赖 FreeKill。

当前能力包括 SGS/1 长度前缀帧、Hello/Ping/Echo、临时用户、房间列表、创建/加入/退出房间、准备状态、完整房间快照、断线清理、房主转移和大厅刷新通知。尚未实现永久账号、游戏规则、Lua、数据库、身份恢复或资源系统。

## 依赖

- CMake 3.20+
- 支持 C++20 的编译器
- Boost 1.83+，需要 Asio 头文件和 Boost.JSON
- Python 3，仅用于自动化 TCP 集成测试

Ubuntu/Debian 对应开发包通常为 `libboost-dev`、`libboost-json-dev` 和 `python3`。本项目不会自动下载安装依赖。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CTest 会启动本机真实 TCP 服务端，分别执行 M1 网络回归和 M2 多客户端房间测试，所有测试均有明确超时。Release 构建可将 `CMAKE_BUILD_TYPE` 改为 `Release`。

## 启动配置

默认仅监听回环地址，适合本机测试：

```bash
./build/cardgame-server
# 监听 127.0.0.1:18765
```

可通过环境变量覆盖：

```bash
SGS_BIND_ADDRESS=127.0.0.1 SGS_PORT=18765 ./build/cardgame-server
```

只有显式设置 `SGS_BIND_ADDRESS=0.0.0.0` 才会监听所有 IPv4 网卡。这样做会把未包含认证和加密的 M1 服务暴露给同网段或公网；跨设备联调前应配置防火墙，只开放可信网络，并避免公网直接暴露。

按 `Ctrl+C` 或发送 `SIGTERM` 可安全停止。端口必须为 1～65535。

## Windows 客户端联调

同机环境连接 `127.0.0.1:18765`。跨设备时，服务端显式监听局域网地址或 `0.0.0.0`，Windows 客户端连接 Linux 主机的局域网 IPv4 地址与配置端口。连接后必须在 10 秒内按 [SGS/1 协议](docs/protocol.md)发送 Hello，再发送 EnterLobby 创建临时身份；TCP 字节流必须实现 4 字节大端长度头，不能假设一次读取就是一条完整消息。

架构与生命周期见 [docs/architecture.md](docs/architecture.md)，验收状态见 [docs/milestone_status.md](docs/milestone_status.md)。
