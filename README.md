# SgsServer

SgsServer 是独立开发的 Linux 多人在线回合制卡牌游戏服务端。M3-B2 第一部分在既有 SGS/1、大厅房间、资源包同步和 GameIPC/1 基础上，增加了可执行真实 Lua 5.4 的最小独立 Runtime，不依赖 FreeKill。

当前能力包括 SGS/1 长度前缀帧、Hello/Ping/Echo、M2 大厅与房间、Package Manifest v1 / Package Sync v1，以及 GameIPC/1 编解码、双向 sequence、非阻塞管道、背压和子进程异步回收。服务端可从公开 HTTPS/SSH Git 仓库锁定完整 commit 并原子持久化注册表。最小 Lua Runtime 能校验冻结 generation 和 InitGame、加载各包自己的 server entry、注册 2–8 人模式并返回 GameReady；它尚未接入房间开始流程，也未实现正式规则/请求桥接、永久账号、数据库、私有仓库凭据或资源包热加载。

## 依赖

- CMake 3.20+
- 支持 C++20 的编译器
- Boost 1.83+，需要 Asio 头文件和 Boost.JSON
- Git 命令行工具，用于资源包安装和已安装 commit 复核
- Python 3，仅用于自动化 TCP 集成测试

Ubuntu/Debian 对应开发包通常为 `libboost-dev`、`libboost-json-dev` 和 `python3`。本项目不会自动下载安装依赖。

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

CTest 包含 GameIPC 编解码与进程监管、资源包单元/真实 Git 夹具、M1/M2 回归和 Package Sync 真实 TCP 测试，所有测试均有明确超时。Release 构建可将 `CMAKE_BUILD_TYPE` 改为 `Release`。

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

资源包状态目录按 `SGS_PACKAGE_ROOT`、`XDG_STATE_HOME/sgs-server/packages`、`$HOME/.local/state/sgs-server/packages` 的顺序选择。可用独立管理程序注册和切换包：

```bash
SGS_PACKAGE_ROOT=/srv/sgs/packages ./build/sgs-package-admin \
  install https://example.org/packages/base.git <完整小写Git提交哈希>
SGS_PACKAGE_ROOT=/srv/sgs/packages ./build/sgs-package-admin enable org.example.base
SGS_PACKAGE_ROOT=/srv/sgs/packages ./build/sgs-package-admin list
```

管理程序只接受公开 `https://` 或 `ssh://` URL，不接受 URL 内嵌凭据。它使用参数数组调用 Git，不经过 shell，并禁用仓库 hook、交互提示及 `ext`/生产 `file` 协议。多个管理进程通过 package root 内的跨进程文件锁串行执行完整的“重读—验证—修改—持久化”事务；注册表用进程唯一临时文件原子替换，安装或启用失败不会替换旧有效配置。

**当前运行中的服务端不会监视外部管理程序写入。执行 install/enable/disable/set-enabled 后，必须在维护窗口正常重启服务端，才能载入新清单。** 不要通过杀进程或直接编辑 `registry.json` 代替正常管理和重启。进程内清单切换接口会立即清除 Ready 并发送变更通知，但 M3-A 尚未提供对应的远程管理入口。

当前 Git 子进程尚无总下载超时、仓库大小、checkout 文件数、单文件大小或磁盘配额。只应注册可信且规模受控的公开仓库，并由部署环境提供进程级超时和磁盘监控。

只有显式设置 `SGS_BIND_ADDRESS=0.0.0.0` 才会监听所有 IPv4 网卡。这样做会把未包含认证和加密的 M1 服务暴露给同网段或公网；跨设备联调前应配置防火墙，只开放可信网络，并避免公网直接暴露。

按 `Ctrl+C` 或发送 `SIGTERM` 可安全停止。端口必须为 1～65535。

## Windows 客户端联调

同机环境连接 `127.0.0.1:18765`。跨设备时，服务端显式监听局域网地址或 `0.0.0.0`，Windows 客户端连接 Linux 主机的局域网 IPv4 地址与配置端口。连接后必须在 10 秒内按 [SGS/1 协议](docs/protocol.md)发送 Hello，再完成 GetPackageManifest / PackageReady；即使服务器清单为空也不能跳过。之后才可 EnterLobby。旧 M2 客户端仍可 Hello/Ping/Echo，但 EnterLobby 会得到 `PACKAGE_PROTOCOL_REQUIRED`。TCP 字节流必须实现 4 字节大端长度头，不能假设一次读取就是一条完整消息。

架构与生命周期见 [docs/architecture.md](docs/architecture.md)，GameIPC/1 内部接口见 [docs/game_ipc.md](docs/game_ipc.md)，验收状态见 [docs/milestone_status.md](docs/milestone_status.md)。
