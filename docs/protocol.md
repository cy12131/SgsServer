# SGS/1 TCP 通信协议

## 1. 传输与帧格式

SGS/1 使用 TCP。每条消息格式为：

```text
+---------------------------+------------------------------+
| 4-byte unsigned big-endian| UTF-8 JSON body              |
| body length               | exactly body length bytes    |
+---------------------------+------------------------------+
```

长度只计算 JSON 消息体，不含 4 字节头。消息体允许 1～65536 字节，不得含 UTF-8 BOM。接收方必须处理 TCP 半包、粘包和连续帧，不能把一次 `recv` 等同于一条消息。长度为 0、超过上限、无效 UTF-8 或无法解析的 JSON 会导致服务端关闭连接并记录日志。

## 2. 公共 JSON 结构

```json
{
  "protocol": 1,
  "type": "Ping",
  "request_id": 2,
  "data": {}
}
```

四个字段均必填：`protocol` 是整数且当前只能为 1；`type` 是区分大小写的字符串；`request_id` 是 1～2147483647 的整数；`data` 是对象。缺少字段或字段类型、范围非法属于非法消息。

## 3. 连接状态

新连接处于 **WaitingHello**。客户端必须在 TCP 建立后 10 秒内发送合法 Hello，否则服务端关闭连接。握手成功后进入 **Ready**，才可发送 Ping 和 Echo。

WaitingHello 中先发送其他格式正确的请求会收到 `HANDSHAKE_REQUIRED`，连接保持可用。Ready 中再次发送 Hello 会收到 `INVALID_MESSAGE`。不支持的协议版本在 request_id 合法时收到 `UNSUPPORTED_PROTOCOL`，随后连接关闭。

## 4. Hello / HelloAck

请求：

```json
{"protocol":1,"type":"Hello","request_id":1,"data":{"client_name":"SgsClient","client_version":"0.1.0-dev"}}
```

`client_name` 和 `client_version` 必须是字符串，当前不要求客户端与服务端软件版本相同。

响应：

```json
{"protocol":1,"type":"HelloAck","request_id":1,"data":{"server_name":"SgsServer","server_version":"0.1.0-dev"}}
```

## 5. Ping / Pong

请求的 `data` 必须为空对象：

```json
{"protocol":1,"type":"Ping","request_id":2,"data":{}}
```

响应：

```json
{"protocol":1,"type":"Pong","request_id":2,"data":{}}
```

## 6. Echo / EchoResult

请求：

```json
{"protocol":1,"type":"Echo","request_id":3,"data":{"text":"你好，SgsServer！"}}
```

`text` 必须是字符串。响应保留相同 request_id 与 Unicode 文本：

```json
{"protocol":1,"type":"EchoResult","request_id":3,"data":{"text":"你好，SgsServer！"}}
```

## 7. Error

```json
{
  "protocol": 1,
  "type": "Error",
  "request_id": 4,
  "data": {
    "code": "UNKNOWN_MESSAGE_TYPE",
    "message": "Unsupported message type"
  }
}
```

| code | 条件 | 是否关闭 |
| --- | --- | --- |
| `UNSUPPORTED_PROTOCOL` | 合法 request_id 的消息使用非 1 协议版本 | 写完 Error 后关闭 |
| `HANDSHAKE_REQUIRED` | Hello 前发送其他格式正确的请求 | 否 |
| `INVALID_MESSAGE` | 重复 Hello 或已识别消息的业务字段不合法 | 否 |
| `UNKNOWN_MESSAGE_TYPE` | Ready 状态收到格式正确但未知的 type | 否 |

无法取得合法 request_id 的公共字段错误、无效 JSON/UTF-8 和非法帧长度可以直接关闭，不保证 Error 响应。任何协议错误都只影响对应 Session，不应导致服务端进程退出。

## 8. M1-S 限制

SGS/1 M1-S 没有 TLS、压缩、认证、登录、房间、游戏消息、重连恢复或复杂版本协商。服务默认只监听 `127.0.0.1:18765`；跨设备联调需显式修改监听地址，并自行限制网络暴露范围。

## 9. M2 临时用户与大厅

M2 不改变 `protocol: 1`。客户端完成 Hello 后可发送 EnterLobby：

```json
{"protocol":1,"type":"EnterLobby","request_id":10,"data":{"nickname":"玩家A"}}
```

响应创建仅在当前服务进程生命周期有效的用户身份：

```json
{"protocol":1,"type":"EnterLobbyResult","request_id":10,"data":{"user_id":1001,"nickname":"玩家A"}}
```

昵称去除首尾 Unicode 空白后必须为 1～20 个 Unicode 字符，且不能包含控制字符。昵称允许重复；身份判断只使用服务端分配的正整数 user_id。每条连接最多创建一个身份，断线后身份立即失效，重连必须重新 EnterLobby。

获取房间列表：

```json
{"protocol":1,"type":"ListRooms","request_id":11,"data":{}}
```

```json
{"protocol":1,"type":"ListRoomsResult","request_id":11,"data":{"rooms":[{"room_id":1,"name":"测试房间","owner_nickname":"玩家A","player_count":1,"max_players":8,"status":"waiting"}]}}
```

## 10. 创建、加入与退出房间

创建请求中房间名称去除首尾空白后必须为 1～40 个 Unicode 字符且不含控制字符，max_players 必须为 2～8：

```json
{"protocol":1,"type":"CreateRoom","request_id":12,"data":{"name":"测试房间","max_players":8}}
```

```json
{"protocol":1,"type":"CreateRoomResult","request_id":12,"data":{"room_id":1}}
```

创建者自动成为房主、占用最小可用座位且 ready 为 false。初始 revision 为 1。服务运行期间同时最多存在 100 个房间。

加入请求与响应：

```json
{"protocol":1,"type":"JoinRoom","request_id":13,"data":{"room_id":1}}
```

```json
{"protocol":1,"type":"JoinRoomResult","request_id":13,"data":{"room_id":1}}
```

服务端验证房间存在、未满且用户当前不在任何房间后，分配 1～max_players 中最小可用座位。既有玩家座位不变化。

退出请求与响应：

```json
{"protocol":1,"type":"LeaveRoom","request_id":15,"data":{}}
```

```json
{"protocol":1,"type":"LeaveRoomResult","request_id":15,"data":{}}
```

房主退出或断线时，按加入顺序最早的剩余成员成为房主，且其 ready 变为 false。最后一人离开后房间立即删除。其他玩家的 ready 不因成员加入、退出或断线而重置。

## 11. 准备状态

```json
{"protocol":1,"type":"SetReady","request_id":14,"data":{"ready":true}}
```

```json
{"protocol":1,"type":"SetReadyResult","request_id":14,"data":{"ready":true}}
```

只有非房主成员可设置准备状态；房主的 ready 永远为 false。ready 实际发生变化时 revision 增加；重复设置相同值成功返回，但 revision 不变。

## 12. RoomSnapshot 通知

创建、加入、退出、断线清理或准备操作成功后，服务端向变化后的当前房间成员发送完整快照。通知 request_id 固定为 0：

```json
{
  "protocol": 1,
  "type": "RoomSnapshot",
  "request_id": 0,
  "data": {
    "room_id": 1,
    "revision": 2,
    "name": "测试房间",
    "max_players": 8,
    "status": "waiting",
    "owner_id": 1001,
    "players": [
      {"user_id":1001,"nickname":"玩家A","seat":1,"ready":false,"is_owner":true},
      {"user_id":1002,"nickname":"玩家B","seat":2,"ready":false,"is_owner":false}
    ]
  }
}
```

成员或实际准备状态变化时 revision 单调增加。M2 只使用完整快照，不使用增量同步，房间状态固定为 `waiting`。

## 13. RoomListChanged 通知

房间创建、加入、退出、删除或断线引起列表变化后，服务端只向“已进入大厅且当前不在房间”的连接发送：

```json
{"protocol":1,"type":"RoomListChanged","request_id":0,"data":{}}
```

通知不携带列表，客户端收到后自行请求 ListRooms。

成功业务操作的同一 Session FIFO 顺序固定为：先发送对应 Result，再发送 RoomSnapshot 或 RoomListChanged。退出者已经脱离原房间，因此不接收原房间的新快照。

## 14. M2 错误码

M2 沿用第 7 节 Error 格式，并增加：

| code | 条件 |
| --- | --- |
| `LOBBY_REQUIRED` | 未进入大厅就执行大厅/房间操作 |
| `ALREADY_IN_LOBBY` | 同一连接重复 EnterLobby |
| `INVALID_NICKNAME` | 昵称为空、过长或含控制字符 |
| `INVALID_ROOM_DATA` | 房间名称、容量或 room_id 字段非法 |
| `ROOM_NOT_FOUND` | 指定房间不存在 |
| `ROOM_FULL` | 房间人数达到 max_players |
| `ALREADY_IN_ROOM` | 用户已经属于一个房间 |
| `NOT_IN_ROOM` | 用户当前不在房间 |
| `PERMISSION_DENIED` | 房主尝试 SetReady 等身份不允许操作 |
| `RESOURCE_LIMIT` | 已达到 100 个房间上限 |

业务错误不改变任何有效状态，原则上不关闭连接。客户端业务请求仍必须使用 1～2147483647 的 request_id；0 仅供服务端通知，客户端发送 0 属于 SGS/1 公共字段错误，按非法消息关闭连接。

## 15. M2 限制

M2 临时用户不持久化，没有密码、恢复令牌或跨连接身份恢复；房间不能开始游戏。尚无 Lua、卡牌/技能、数据库、资源包同步、TLS、恶意用户防护体系或复杂订阅系统。
