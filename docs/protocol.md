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
