# 服务端架构说明（M1-S）

## 进程入口与所有权

`main` 只创建 `Application` 并转交退出码。`Application::run()` 读取监听环境变量，在栈上创建 `TcpServer` 并进入单线程 Boost.Asio 事件循环。`TcpServer` 拥有 `io_context`、acceptor、信号监听器和活动 Session 集合；收到 SIGINT/SIGTERM 后关闭 acceptor 与所有 Session，事件循环返回，对象再按 RAII 顺序析构。

## 网络与协议边界

- **TcpServer**：绑定地址和端口、异步 accept、持有活动 Session，并控制启动和停止。
- **Session**：独占一个 TCP socket，维护 WaitingHello/Ready 状态、10 秒定时器、帧读取和异步写队列。
- **MessageCodec**：验证 UTF-8 与公共 JSON 字段，在完整消息体和强类型 `Message` 间转换，并编码长度前缀。
- **MessageRouter**：根据当前会话状态和 `type` 生成 HelloAck、Pong、EchoResult 或 Error；不持有 socket。

房间、游戏、Lua、认证和数据库均不属于这些类，也未在 M1-S 创建占位抽象。

未来资源包管理是独立于网络协议层的上层能力：服务端与客户端将分别依据后续统一设计的清单、远程仓库、版本、完整性和安全策略管理资源。当前 Hello 只建立 SGS/1 会话，不强制绑定游戏规则包或资源包版本；M1-S 不定义资源同步消息，也不创建 PackageManager 或 Downloader。

## Session 生命周期

accept 成功后，`TcpServer` 创建 `shared_ptr<Session>` 并保存到活动集合。Session 的异步回调捕获自身，保证操作完成前对象存活；关闭回调通知服务器从集合移除。`close()` 幂等地取消计时器、shutdown/close socket、清空待写数据并解除服务器所有权，避免重复关闭与悬空回调。

建立连接即进入 WaitingHello 并启动 10 秒定时器。合法 Hello 取消定时器并进入 Ready。超时、无效长度、无效 UTF-8、JSON 解析失败、非法公共字段、读写错误或对端断开都会安全释放 Session。

## 消息收发链路

读取先用 `async_read` 精确收满 4 字节头，再按大端长度精确收满消息体。因此 TCP 半包会继续等待，粘包或连续帧则在下一轮读取中依次消费。完整体经过 MessageCodec 后交给 MessageRouter。

每个 Session 只有一个待写队列。新响应在队尾保存完整帧；仅当队列由空变为非空时启动 `async_write`。完成回调移除队首并启动下一项，所以同一 socket 永远只有一个异步写操作，且缓冲区在完成前保持有效。

## 单线程模型

M1-S 只调用一次 `io_context::run()`，accept、Session 状态变化、写队列和停止动作在同一事件线程串行执行，不需要锁或 strand。未来若引入多线程，必须重新审查所有共享状态并明确串行化策略。

## 服务端权威边界与当前限制

未来 Game 模块持有权威对局状态，Room 只组织参与者与对局生命周期，Lua 只承载受控规则执行；客户端只提交意图。未来资源包层负责清单和包生命周期，但不侵入基础帧、Session 或路由职责。M1-S 尚未实现这些模块，也没有 TLS、认证、限流、持久化、资源包管理、WebSocket/HTTP、多线程工作池或复杂版本协商。
