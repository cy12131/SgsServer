#!/usr/bin/env python3
"""SGS/1 真实 TCP 集成测试，仅使用 Python 标准库。"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def encode(message):
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return struct.pack("!I", len(body)) + body


def request(message_type, request_id, data, protocol=1):
    return {
        "protocol": protocol,
        "type": message_type,
        "request_id": request_id,
        "data": data,
    }


def receive_exact(connection, size):
    chunks = bytearray()
    while len(chunks) < size:
        chunk = connection.recv(size - len(chunks))
        if not chunk:
            raise AssertionError("connection closed before a complete frame arrived")
        chunks.extend(chunk)
    return bytes(chunks)


def receive_message(connection):
    length = struct.unpack("!I", receive_exact(connection, 4))[0]
    assert 1 <= length <= 65536
    return json.loads(receive_exact(connection, length).decode("utf-8"))


def assert_error(message, request_id, code):
    assert message["protocol"] == 1
    assert message["type"] == "Error"
    assert message["request_id"] == request_id
    assert message["data"]["code"] == code


def connect(port, timeout=3.0):
    connection = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    connection.settimeout(timeout)
    return connection


def wait_for_server(port):
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            return connect(port)
        except OSError:
            time.sleep(0.05)
    raise AssertionError("server did not begin listening within five seconds")


def hello(connection, request_id=1):
    connection.sendall(
        encode(
            request(
                "Hello",
                request_id,
                {"client_name": "SgsClient", "client_version": "0.1.0-dev"},
            )
        )
    )
    response = receive_message(connection)
    assert response == {
        "protocol": 1,
        "type": "HelloAck",
        "request_id": request_id,
        "data": {"server_name": "SgsServer", "server_version": "0.1.0-dev"},
    }


def assert_closed(connection):
    connection.settimeout(3.0)
    assert connection.recv(1) == b""


def run_tests(port):
    # TCP 连接成功；立即异常断开后，服务端仍能接受后续连接。
    probe = wait_for_server(port)
    probe.close()

    # 单帧拆分发送（半包）、Hello、粘包/连续消息、Ping 和中文 Echo。
    with connect(port) as connection:
        hello_frame = encode(
            request(
                "Hello",
                1,
                {"client_name": "SgsClient", "client_version": "0.1.0-dev"},
            )
        )
        for byte in hello_frame:
            connection.sendall(bytes((byte,)))
        assert receive_message(connection)["type"] == "HelloAck"

        connection.sendall(
            encode(request("Ping", 2, {}))
            + encode(request("Echo", 3, {"text": "你好，SgsServer！"}))
        )
        assert receive_message(connection) == {
            "protocol": 1,
            "type": "Pong",
            "request_id": 2,
            "data": {},
        }
        echo = receive_message(connection)
        assert echo["type"] == "EchoResult"
        assert echo["request_id"] == 3
        assert echo["data"]["text"] == "你好，SgsServer！"

    # 多客户端同时建立独立会话。
    with connect(port) as first, connect(port) as second:
        first.sendall(encode(request("Hello", 10, {"client_name": "A", "client_version": "1"})))
        second.sendall(encode(request("Hello", 20, {"client_name": "B", "client_version": "1"})))
        assert receive_message(first)["request_id"] == 10
        assert receive_message(second)["request_id"] == 20

    # 不支持的协议应返回错误并在写完后关闭。
    with connect(port) as connection:
        connection.sendall(encode(request("Hello", 30, {}, protocol=2)))
        assert_error(receive_message(connection), 30, "UNSUPPORTED_PROTOCOL")
        assert_closed(connection)

    # 未握手业务消息返回错误，但连接仍可继续完成 Hello。
    with connect(port) as connection:
        connection.sendall(encode(request("Ping", 40, {})))
        assert_error(receive_message(connection), 40, "HANDSHAKE_REQUIRED")
        hello(connection, 41)

    # 超长帧、无效 JSON、无效 UTF-8 与非法必填字段均安全关闭。
    invalid_frames = [
        struct.pack("!I", 65537),
        struct.pack("!I", 4) + b"{bad",
        struct.pack("!I", 2) + b'"\xff',
        encode({"protocol": 1, "type": "Ping", "request_id": 50}),
    ]
    for invalid_frame in invalid_frames:
        with connect(port) as connection:
            connection.sendall(invalid_frame)
            assert_closed(connection)

    # 重复 Hello、未知类型及业务字段错误均返回规定的 Error。
    with connect(port) as connection:
        hello(connection, 60)
        connection.sendall(encode(request("Hello", 61, {"client_name": "A", "client_version": "1"})))
        assert_error(receive_message(connection), 61, "INVALID_MESSAGE")
        connection.sendall(encode(request("FutureMessage", 62, {})))
        assert_error(receive_message(connection), 62, "UNKNOWN_MESSAGE_TYPE")
        connection.sendall(encode(request("Echo", 63, {"text": 123})))
        assert_error(receive_message(connection), 63, "INVALID_MESSAGE")

    # 空闲未握手连接在十秒期限到达后由服务端关闭。
    with connect(port, timeout=13.0) as connection:
        started = time.monotonic()
        connection.settimeout(13.0)
        assert connection.recv(1) == b""
        elapsed = time.monotonic() - started
        assert 9.0 <= elapsed <= 12.5, elapsed


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: network_integration_test.py SERVER_EXECUTABLE")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="sgs-m1-package-root-") as package_root:
        environment = os.environ.copy()
        environment["SGS_BIND_ADDRESS"] = "127.0.0.1"
        environment["SGS_PORT"] = str(port)
        environment["SGS_PACKAGE_ROOT"] = package_root
        server = subprocess.Popen(
            [sys.argv[1]],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            run_tests(port)
            server.send_signal(signal.SIGTERM)
            return_code = server.wait(timeout=5.0)
            stdout, stderr = server.communicate()
            assert return_code == 0, (return_code, stdout, stderr)
            assert "CardGameServer 0.1.0-dev" in stdout
            assert "started successfully" in stdout
            assert "Stopping CardGameServer." in stdout
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=2.0)


if __name__ == "__main__":
    main()
