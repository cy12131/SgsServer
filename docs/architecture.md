# 服务端架构说明（M3-B1）

## 进程入口与所有权

`main` 只创建 `Application` 并转交退出码。`Application::run()` 读取监听和资源包状态目录环境变量，在栈上创建 `TcpServer` 并进入单线程 Boost.Asio 事件循环。`TcpServer` 依次拥有 `PackageManager`、`PackageSyncService`、`LobbyService` 与网络对象；收到 SIGINT/SIGTERM 后关闭 acceptor 与所有 Session，事件循环返回，对象再按 RAII 顺序析构。

## 网络与协议边界

- **TcpServer**：绑定地址和端口、异步 accept、持有活动 Session，并控制启动和停止。
- **Session**：独占一个 TCP socket，维护 WaitingHello/Ready 状态、10 秒定时器、帧读取和异步写队列。
- **MessageCodec**：验证 UTF-8 与公共 JSON 字段，在完整消息体和强类型 `Message` 间转换，并编码长度前缀。
- **MessageRouter**：根据当前会话状态和 `type` 分发基础请求或 M2 业务请求；不持有 socket。
- **PackageManager**：在网络和大厅之外负责 Git 安装、Manifest 校验、依赖解析、注册表事务与活动清单。
- **PackageSyncService**：只保存 `session_id → manifest_id` 的同步确认，并实现 Package Sync v1 请求和清单变化失效。
- **UserManager**：以 Session ID 建立临时用户身份，分配运行期唯一 user_id，记录可空的 room_id。
- **RoomManager**：持有 Room、稳定座位、加入顺序、准备状态和 revision，实现创建、加入、退出、房主转移与空房删除。
- **LobbyService**：验证 M2-P 业务字段，协调 UserManager 与 RoomManager，并生成 Result 和定向通知列表。

GameIPC 与游戏子进程不属于上述网络业务类；Lua、永久认证和数据库同样不属于这些类。

资源包管理不写入 LobbyService，也不改变 Hello 内容。PackageManager 的输出通过 PackageSyncService 暴露给路由；LobbyService 只在路由确认当前 Session 已同步后才接收 EnterLobby/CreateRoom/JoinRoom。客户端直接从清单中的仓库获取资源，服务端不充当下载代理。

## 独立游戏进程与 GameIPC 边界

`GameProcessManager` 是未接入 TcpServer/RoomManager 的独立核心组件。它按活动游戏拥有一个子进程和两条单向非阻塞匿名管道，启动时冻结 game_id、room_id、manifest_id、有序 packages、mode、roster/seats 及 child identity。子进程不持有 Session、socket 或大厅对象，不能直接发送 SGS/1 或修改房间状态。

`GameIpcCodec` 与 SGS/1 MessageCodec 完全分离，严格实现 GameIPC/1 的 4 字节大端长度、1 MiB 正文上限、九种消息、方向及 Schema 等价验证。父→子和子→父 sequence 各自从 1 连续递增。GameReady 必须回显启动绑定；来自子进程的 token、recipient 和 roster 关联信息由父进程再次校验。IPC 有效只表示传输契约有效，不代表动作获得业务授权。

父端描述符接入同一个 Boost.Asio 事件循环，读取、写入、waitpid 检测与停止升级均异步执行。写队列默认限制为 4 MiB 或 256 帧，越界只终止异常游戏。停止路径依次发送 Shutdown、SIGTERM、SIGKILL，并在 waitpid 成功后恰好回调一次。Runtime CLI/FD 契约、Lua 沙箱、只读 generation、资源限制及游戏视图路由留给 B2～B4。

## 资源包安装与事务边界

`sgs-package-admin install` 用参数数组启动 Git，不拼接 shell 命令。生产只允许不含内嵌凭据的 HTTPS 或 SSH URL；关闭系统/用户 Git 配置、交互提示、仓库 hook 与 `ext` 协议。下载进入状态目录下的独立 staging，随后验证完整小写 commit、根目录 `manifest.json` 原始字节 SHA-256、Schema 等价约束、UTF-8、声明路径与符号链接边界。全部成功后才将 checkout 原子移动到不可变的 `installed/<id>/<commit>`。

注册表记录仓库 URL、commit、Manifest 摘要、相对安装目录和 enabled 状态。每个读取操作取得共享 `flock`，每个变更操作取得排他 `flock`，并在锁内重新读取最新注册表；锁覆盖 Git 下载、验证、候选状态计算和持久化，避免两个管理进程以旧快照互相覆盖。写入使用包含 PID 与进程内原子计数的临时文件，以 `O_EXCL` 创建、fsync 后 rename；启用操作先计算完整依赖闭包、版本范围、循环、重复 ID、确定性拓扑顺序、RFC 8785 `manifest_id` 和实际 SGS/1 帧长度，验证通过后才替换注册表及内存活动清单。启动或 reload 时会复核 checkout HEAD、index/worktree（包括未跟踪及 ignored 业务文件）、Manifest 摘要和 `installed/<id>/<commit>` canonical 归属；失败保留旧内存状态。`manifest.json` 必须是包根内的普通非符号链接文件，并以 `O_NOFOLLOW` 打开。

这些校验假定资源包状态根只由遵守 `.registry.lock` 的管理工具和服务进程维护，可拒绝安装失败、磁盘污染及重启加载时的非规范目录；它们不构成对拥有同等本机文件写权限攻击者的完整文件系统沙箱。检查与后续使用之间仍可能被该类进程竞争修改，正式 Runtime 还需以只读 generation、受限文件系统权限和 owner package root 解析进一步收紧边界。

管理程序与网络服务是独立进程。当前 M3-A 不监视注册表文件；外部 CLI 更新后，正在运行的服务端必须正常重启，才会载入新清单。若未来把管理入口接入服务进程，必须在工作线程完成 Git/磁盘校验，再回到 Asio 线程调用现有原子切换接口，不得阻塞网络事件循环。

## 清单与 Session 同步生命周期

活动包按依赖拓扑顺序加载；同一拓扑层按包 ID 排序，清单序列及每个包的依赖 ID 均确定性排序。`manifest_id` 是 Package Sync v1 指定字段数组的 RFC 8785 规范化 JSON SHA-256；`manifest_sha256` 则是仓库中 `manifest.json` 原始字节 SHA-256，两者不可混用。

TCP Hello 成功并不代表资源包 Ready。每个新 Session 默认 NotReady，必须获取清单并用当前 `manifest_id` 显式 PackageReady，空清单亦同。状态只由 PackageSyncService 以数值 session_id 持有，断线立即删除。进程内活动清单改变时先清空所有确认，再由 TcpServer 向已完成 Hello 的连接广播 `PackageManifestChanged`。已在房间的用户不会被 M3-A 强制移出，现有身份和房间状态仍由 LobbyService 管理；新的 EnterLobby/CreateRoom/JoinRoom 会受门禁，实际对局热切换留待后续设计。

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

未来 Game Runtime 持有权威对局状态，Room 只组织参与者与对局生命周期，Lua 只承载受控规则执行；客户端只提交意图。M3-B1 尚未把进程管理接入房间、加载包内 Lua/QML、实现正式 Runtime API、限制子进程 CPU/内存/文件系统，或提供游戏视图路由。私有 Git 凭据、在线热加载、TLS、永久认证、限流、数据库、WebSocket/HTTP 和多线程工作池也未实现。Git 下载尚无总超时；也未限制仓库字节数、checkout 文件数、单文件大小、staging/installed 总磁盘用量或包数量以外的资源配额。
