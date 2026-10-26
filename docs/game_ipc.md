# GameIPC/1 与独立游戏进程（M3-B1）

本模块实现 SgsArchitecture `ca3e9bab9084050945051b181de5cea55413f9d7` 中 M3-B1 的进程与通信基础。它与客户端 SGS/1 完全分离，当前也没有接入房间“开始游戏”流程或执行 Lua 规则。

## 帧、Schema 与 sequence

GameIPC 帧为四字节无符号大端正文长度加 UTF-8 JSON，正文必须为 1～1,048,576 字节。`GameIpcFrameDecoder` 可增量接收任意分片并一次拆出多个连续帧；长度为零或超过上限立即以 `IPC_FRAME_INVALID` 拒绝。

`GameIpcCodec` 只接受 `ipc_version: "1"` 和契约定义的九种消息：InitGame、GameReady、PlayerAction、PlayerRequest、PlayerReply、GameOutput、GameFinished、GameFailed、Shutdown。它执行等价于正式 Schema 的严格字段、类型、范围、枚举和 additionalProperties 检查，并额外执行 uint64 上限、玩家/座位唯一性、包 ID 唯一性、reply_mode 数量关系及 recipient 子集等语义检查。非法 UTF-8、重复 JSON key、未知消息和方向错误均受控拒绝；业务 `data` 不能绕过外层验证。

每条父→子和子→父通道各自拥有一个 sequence 实例，从字符串 `"1"` 连续递增。重复、倒退、跳号、零、前导零、超过 UINT64_MAX 或回绕均产生 `IPC_SEQUENCE_INVALID`，一个方向不会推进另一个方向。

## 启动记录与进程边界

`GameProcessManager` 在 fork 前按值固定 `game_id → {room_id, manifest_id, ordered packages, mode_id, roster/seats, seed, settings, init token, package root}`。同一管理器生命周期内 game_id 不可复用；启动后额外记录 PID 形式的 child identity。InitGame 只从该不可变记录生成，GameReady 必须回显 init token、mode 和完整有序包集合，任何不一致均以 `IPC_BINDING_MISMATCH` 终止目标进程。

B1 内部 Runtime CLI 采用 Implementation Candidate：

```text
<executable> --game-id <id> --package-root <absolute-path> \
  --ipc-read 3 --ipc-write 4 [test/runtime-specific arguments]
```

父进程不用 shell，而是使用绝对可执行路径和 argv 调用 `execve`。环境缩减为 PATH、LANG 和 TZ，cwd 显式指定；只把 FD 3/4 两个目标管道及标准 FD 保留给子进程，其他描述符通过 close_range 或兼容回退关闭。Runtime CLI、标准输出/错误日志管道和只读 generation 挂载仍需在 B2 接入真实 Runtime 时冻结；替换成本局限于启动器参数构造，不影响 codec 和异步通道。

## 非阻塞 I/O、背压和回收

两条单向匿名管道均使用 `O_NONBLOCK|O_CLOEXEC`，父端由 Boost.Asio `posix::stream_descriptor` 驱动。`async_read_some` 加增量 decoder 处理 partial read、EAGAIN、EINTR、粘包和 EOF；单一异步写操作按队首顺序完成 partial write。SIGPIPE 被忽略并转换为 Asio broken_pipe，只失败对应游戏。

每个父→子写队列默认最多 4 MiB 或 256 帧，先达到者生效。超过限制不再接收新帧，记录 `IPC_BACKPRESSURE`，关闭目标管道并进入受控回收，不能无限累积。限制可在测试中调小，但生产默认值固定遵守契约。

进程退出由短周期异步 timer 调用 `waitpid(..., WNOHANG)` 检测，网络事件循环不会阻塞等待。正常停止先排队 Shutdown；宽限期后发送 SIGTERM，再超时才发送 SIGKILL。初始化超时、非法协议、管道关闭、非零退出、信号退出和 exec 失败均清理 descriptor/timer，并且退出回调和 reap 只执行一次。PID 在 waitpid 回收前不会被其他进程复用，因此信号只发送给当前记录的子进程。

异步操作所需的 map、进程状态和辅助方法位于引用计数的内部 `State`；每个 read/write/timer/reap 回调直接持有 `shared_ptr<State>`，不捕获 `GameProcessManager` 裸指针。管理器析构只执行幂等 shutdown；已排队回调中的 State 会存活到回调返回，并在看到 shutting_down 后停止递归排队。若析构时仍有活动子进程，兜底路径发送 SIGKILL，并由只等待该 PID 的独立 reaper 回收，避免阻塞 Asio 线程或留下 zombie。正常运行仍应优先调用异步停止流程，而不是依赖析构兜底。

当前服务端只有一个 `io_context::run()` 线程；GameProcessManager 的公共方法、析构和内部 State 变化必须在该线程串行执行。引用计数解决对象生命周期，不把 State 变成线程安全容器。未来若启用多线程 `io_context`，接入前必须用 strand 或等价同步覆盖所有公共调用和回调。

## 当前安全边界与后续接口

- 测试子进程只持有 GameIPC 管道，不取得 Session、socket、LobbyService 或 RoomManager。
- 父进程生成 game_id 和 sequence，并验证子进程的 game_id、token、包集合以及 PlayerRequest/GameOutput recipients 均属于冻结 roster。
- IPC 成功只代表传输和外层 Schema 有效，不代表动作合法或规则安全。
- `GameProcessManager` 当前作为独立核心组件构建和测试，尚未由 TcpServer/RoomManager 创建真实对局。
- B2 需要提供受限 Lua Runtime、只读 generation、最终 CLI/FD 协议、stdout/stderr 限流净化，以及包入口加载和真实 GameReady。
- B1 stub 不写 stdout/stderr；当前子进程仍继承父进程的 FD 1/2。B2 启动真实 Runtime 前必须改为独立异步日志通道：持续排空，单条截断，按局限流，敏感字段净化，并使用有界 sink 队列。队列满时丢弃新日志并累计一次摘要，不能停止读取管道或无限扩张内存。
- B3/B4 仍需完成 Request token 状态表与 deadline、正式 Lua 事件/协程桥接、定向视图路由、CPU/内存/FD/文件系统限制和失败日志净化。

测试使用专用 `cardgame-game-process-stub`，覆盖合法 Init/GameReady、分片读写、非法 JSON/类型/长度/sequence、绑定不一致、初始化超时、崩溃、EOF、字节/帧背压、重复停止、SIGTERM→SIGKILL、多个并发子进程、FD 继承和 zombie 检查。该 stub 不是 Lua Runtime，也不随生产服务启动。
