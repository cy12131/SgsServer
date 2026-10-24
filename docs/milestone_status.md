# M2-S 里程碑状态报告

状态：**临时用户、大厅、房间和准备状态已实现并通过自动化验收，等待总体架构负责人审核。**

## 工程与 Git

- 正式目录：`/home/cy/Code/SgsServerRepo`
- 开发分支：`feature/m2-lobby-room-server`
- 基线：`v0.2.0-m1` / main 合并提交 `54f4d1f`
- 技术栈保持 C++20、Boost.Asio、Boost.JSON、CMake 和 CTest
- 未引入新第三方依赖

## 所有权与模块边界

TcpServer 拥有 LobbyService 和 `session_id → shared_ptr<Session>` 映射。UserManager 中的 User 只保存数值 session_id，不反向持有 Session，因此没有循环 shared_ptr。LobbyService 协调 UserManager 与 RoomManager；MessageRouter 只做消息分发，Session 仍只处理网络生命周期、帧和 FIFO 写队列。

RoomManager 使用按 room_id 排序的房间映射。Room 保存 owner_id、max_players、revision 和按加入顺序排列的 RoomPlayer vector；seat 在 1～max_players 中选择最小空位，现有成员不移动。

## 主要执行链

- EnterLobby：Router → LobbyService 校验并规范化昵称 → UserManager 分配运行期唯一 user_id → EnterLobbyResult。
- CreateRoom：验证身份、名称、容量、单房约束和 100 房限制 → RoomManager 创建 revision=1 的房间并加入房主 → CreateRoomResult → RoomSnapshot → 大厅 RoomListChanged。
- JoinRoom：验证身份、room_id、存在性、容量和单房约束 → 分配最小空座并增加 revision → JoinRoomResult → 当前成员 RoomSnapshot → 大厅 RoomListChanged。
- SetReady：验证成员与非房主身份 → 实际状态变化时增加 revision → SetReadyResult → 当前成员 RoomSnapshot。

同一 Session 的 Result 先进入原有异步写队列，再投递通知，保证 FIFO。跨连接不承诺全局到达顺序。

## 断线清理

Session 关闭时，TcpServer 先调用 LobbyService：若用户在房间则移除成员；房主由加入顺序最早的剩余成员接任且 ready=false；空房删除；随后删除临时用户。变化后的成员收到 RoomSnapshot，大厅用户收到 RoomListChanged。服务停止时执行同样的状态释放，但不广播无意义通知。

## 自动化测试

CTest 包含：

1. `application_smoke`：应用标识回归。
2. `network_integration`：M1 Hello、Ping、中文 Echo、帧、错误、超时与停止回归。
3. `lobby_room_integration`：真实多客户端 TCP 测试，覆盖重复昵称与唯一 ID、昵称校验、未进大厅拒绝、2/8 人房、列表与通知、加入/满员/重复加入、准备/取消、房主权限、稳定座位、revision、退出/房主转移/空房删除、异常断线、快速交替操作、错误不改状态、客户端 request_id=0 拒绝及 100 房间资源上限。

所有 socket 和 CTest 均设置明确超时，不用长时间 sleep 代替同步。

## 已知限制与未实现范围

- 用户和房间仅存在于内存，进程重启后清空。
- M2 没有身份恢复令牌，重连必须重新 EnterLobby。
- 房间只有 `waiting`，不能开始游戏。
- 未实现游戏规则、Lua、卡牌、技能、数据库、永久认证、资源包同步或客户端代码。
- 尚未进行 M2 Windows/Linux 跨设备联调。

M2-S 到此停止，不进入游戏逻辑或后续阶段。
