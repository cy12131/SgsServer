# 服务端架构说明（M2-S）

## 进程入口与所有权

`main` 只创建 `Application` 并转交退出码。`Application::run()` 读取监听环境变量，在栈上创建 `TcpServer` 并进入单线程 Boost.Asio 事件循环。`TcpServer` 拥有 `io_context`、acceptor、信号监听器和活动 Session 集合；收到 SIGINT/SIGTERM 后关闭 acceptor 与所有 Session，事件循环返回，对象再按 RAII 顺序析构。

## 网络与协议边界

- **TcpServer**：绑定地址和端口、异步 accept、持有活动 Session，并控制启动和停止。
- **Session**：独占一个 TCP socket，维护 WaitingHello/Ready 状态、10 秒定时器、帧读取和异步写队列。
- **MessageCodec**：验证 UTF-8 与公共 JSON 字段，在完整消息体和强类型 `Message` 间转换，并编码长度前缀。
- **MessageRouter**：根据当前会话状态和 `type` 分发基础请求或 M2 业务请求；不持有 socket。
- **UserManager**：以 Session ID 建立临时用户身份，分配运行期唯一 user_id，记录可空的 room_id。
- **RoomManager**：持有 Room、稳定座位、加入顺序、准备状态和 revision，实现创建、加入、退出、房主转移与空房删除。
- **LobbyService**：验证 M2-P 业务字段，协调 UserManager 与 RoomManager，并生成 Result 和定向通知列表。

游戏、Lua、永久认证和数据库均不属于这些类，也未在 M2-S 创建占位抽象。

未来资源包管理是独立于网络协议层的上层能力：服务端与客户端将分别依据后续统一设计的清单、远程仓库、版本、完整性和安全策略管理资源。当前 Hello 只建立 SGS/1 会话，不强制绑定游戏规则包或资源包版本；M1-S 不定义资源同步消息，也不创建 PackageManager 或 Downloader。

## Session 生命周期

accept 成功后，`TcpServer` 创建 `shared_ptr<Session>` 并保存到活动集合。Session 的异步回调捕获自身，保证操作完成前对象存活；关闭回调通知服务器从集合移除。`close()` 幂等地取消计时器、shutdown/close socket、清空待写数据并解除服务器所有权，避免重复关闭与悬空回调。

建立连接即进入 WaitingHello 并启动 10 秒定时器。合法 Hello 取消定时器并进入 Ready。超时、无效长度、无效 UTF-8、JSON 解析失败、非法公共字段、读写错误或对端断开都会安全释放 Session。

TcpServer 为连接分配运行期 Session ID。User 只记录该数值，不持有 Session 指针；TcpServer 是网络 Session 的唯一长期所有者。断开回调先让 LobbyService 清理用户及房间关系，再从 Session 映射移除网络对象，避免 shared_ptr 循环。

## 消息收发链路

读取先用 `async_read` 精确收满 4 字节头，再按大端长度精确收满消息体。因此 TCP 半包会继续等待，粘包或连续帧则在下一轮读取中依次消费。完整体经过 MessageCodec 后交给 MessageRouter。

每个 Session 只有一个待写队列。新响应在队尾保存完整帧；仅当队列由空变为非空时启动 `async_write`。完成回调移除队首并启动下一项，所以同一 socket 永远只有一个异步写操作，且缓冲区在完成前保持有效。

## 单线程模型

M2-S 只调用一次 `io_context::run()`，accept、Session 状态变化、业务状态、写队列和停止动作在同一事件线程串行执行，不需要锁或 strand。未来若引入多线程，必须重新审查所有共享状态并明确串行化策略。

## M2 请求与广播链

业务请求链为：Session 解帧 → MessageRouter 确认 Ready → LobbyService 校验身份和字段 → UserManager/RoomManager 原子式修改状态 → Result 先进入请求 Session 写队列 → TcpServer 按 Session ID 投递通知。因为响应先入队，同一连接严格保持 Result 在 RoomSnapshot/RoomListChanged 之前；不同连接不承诺全局到达顺序。

RoomSnapshot 仅投递当前房间成员。RoomListChanged 只投递已进入大厅且当前不在房间的用户。退出者先脱离房间，所以不会收到旧房间快照，但会作为大厅用户收到列表变化通知。

房间玩家 vector 保持加入顺序；空座位通过扫描 1～max_players 分配最小可用值，既有座位不移动。房主离开时 vector 中最早的剩余玩家接任且 ready 强制为 false。成员或实际准备状态变化使 revision 增加；创建时 revision 为 1。

## 服务端权威边界与当前限制

未来 Game 模块持有权威对局状态，Room 只组织参与者与对局生命周期，Lua 只承载受控规则执行；客户端只提交意图。未来资源包层负责清单和包生命周期，但不侵入基础帧、Session 或路由职责。M2-S 尚未实现游戏开始、TLS、永久认证、限流、持久化、资源包管理、WebSocket/HTTP、多线程工作池或复杂版本协商。
