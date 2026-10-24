# M1-S 里程碑状态报告

状态：**TCP 网络通信与 SGS/1 消息路由已实现并通过自动化验收，等待审核。**

## 工程与环境

- 正式目录：`/home/cy/Code/SgsServerRepo`
- 开发分支：`feature/m1-network-server`
- 编译器：g++ 15.2.0，C++20
- CMake / CTest：4.2.3
- Boost：1.90.0（Ubuntu 已安装的 `libboost-dev`、`libboost-json-dev`）
- JSON：Boost.JSON 1.90.0
- Python：3.14.4，仅用于测试

没有为 M1-S 安装新依赖，也没有复制第三方源码。

## 实现范围

新增 TcpServer、Session、MessageCodec 和 MessageRouter。Application 读取 `SGS_BIND_ADDRESS` / `SGS_PORT`，创建 TcpServer，输出原有程序名称与版本，并在单线程异步事件循环中持续监听。SIGINT/SIGTERM 触发 acceptor、活动连接和事件循环的有序停止。

Session 使用共享所有权覆盖异步回调生命周期，使用 4 字节头和定长体两阶段 `async_read` 处理半包与粘包。每个 Session 的 FIFO 队列保证只有一个 `async_write`，队首缓冲持续到完成回调。10 秒 `steady_timer` 约束 Hello，握手完成即取消。

## 自动化测试

CTest 包含：

1. `application_smoke`：验证稳定的应用名称与版本，不启动长运行事件循环。
2. `network_integration`：由 Python 标准库启动真实服务端并连接 localhost，覆盖 TCP 连接、Hello、Ping/Pong、中文 Echo、连续粘包、逐字节半包、多客户端、异常断开、错误协议版本、握手前业务请求、超长帧、无效 JSON、无效 UTF-8、非法字段、重复 Hello、未知类型、业务字段错误、10 秒握手超时和 SIGTERM 正常停止。

所有 socket 都设置超时；CTest 对整个网络测试设置 30 秒上限，不存在无限等待。

验收命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

实际结果：2/2 通过，0 失败；网络测试耗时约 10.3 秒。由于执行沙箱禁止创建 localhost socket，TCP 测试需要在允许本地网络的执行环境中运行。

## 手工检查

- 审查编译参数包含 `-std=c++20 -Wall -Wextra -Wpedantic`。
- 确认默认绑定 `127.0.0.1:18765`，只有环境变量可改为 `0.0.0.0`。
- 确认无 Lua、Qt、SQLite、HTTP/WebSocket、房间、游戏逻辑或多线程工作池代码。
- 确认 Hello 不强制绑定规则包或资源包版本，且没有 PackageManager、Downloader 或资源同步消息。
- 确认构建产物由 `.gitignore` 排除。

## 已知限制

- 无 TLS、认证、限流和持久化，不应直接暴露到公网。
- 日志仍为最小标准输出/错误输出，尚无结构化日志系统。
- M1 只支持协议版本 1 和三类请求，不做复杂兼容协商。
- Windows 客户端联调尚未执行；本阶段只提供协议和连接说明。
- 远程资源包清单、下载、版本、完整性及安全策略留待后续统一设计，与 M1-S 网络层保持分离。

M1-S 到此停止，不包含房间、游戏规则、Lua、数据库或客户端实现。
