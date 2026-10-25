#!/usr/bin/env python3
"""Package Sync v1 against a real running SGS/1 server with an empty registry."""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


EMPTY_ID = "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"


def encode(message):
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    assert 1 <= len(body) <= 65536
    return struct.pack("!I", len(body)) + body


def receive_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise AssertionError("connection closed before complete SGS/1 frame")
        data.extend(part)
    return bytes(data)


class Client:
    def __init__(self, port):
        self.connection = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.connection.settimeout(3)
        self.next_id = 1

    def close(self):
        self.connection.close()

    def send(self, kind, data):
        request_id = self.next_id
        self.next_id += 1
        self.connection.sendall(encode({
            "protocol": 1,
            "type": kind,
            "request_id": request_id,
            "data": data,
        }))
        return request_id

    def receive(self):
        size = struct.unpack("!I", receive_exact(self.connection, 4))[0]
        assert 1 <= size <= 65536
        return json.loads(receive_exact(self.connection, size).decode("utf-8"))

    def expect(self, kind, request_id):
        message = self.receive()
        assert message["type"] == kind, message
        assert message["request_id"] == request_id, message
        return message

    def hello(self):
        request_id = self.send("Hello", {
            "client_name": "PackageSyncTest",
            "client_version": "0.3.0-m3-test",
        })
        self.expect("HelloAck", request_id)


def wait_for_server(port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            probe = Client(port)
            probe.close()
            return
        except OSError:
            time.sleep(0.02)
    raise AssertionError("server did not listen within five seconds")


def assert_error(message, request_id, code):
    assert message["type"] == "Error", message
    assert message["request_id"] == request_id, message
    assert message["data"]["code"] == code, message


def run_scenario(port):
    client = Client(port)
    try:
        client.hello()

        ping = client.send("Ping", {})
        client.expect("Pong", ping)
        echo = client.send("Echo", {"text": "包同步前仍可诊断"})
        assert client.expect("EchoResult", echo)["data"]["text"] == "包同步前仍可诊断"

        denied = client.send("EnterLobby", {"nickname": "未同步玩家"})
        assert_error(client.receive(), denied, "PACKAGE_PROTOCOL_REQUIRED")

        get_id = client.send("GetPackageManifest", {})
        manifest = client.expect("PackageManifestResult", get_id)
        assert manifest["data"] == {
            "manifest_id": EMPTY_ID,
            "packages": [],
            "sync_timeout_ms": 30000,
        }

        mismatch_id = client.send("PackageReady", {"manifest_id": "b" * 64})
        mismatch = client.expect("PackageReadyResult", mismatch_id)
        assert mismatch["data"]["ready"] is False
        assert mismatch["data"]["error"] == {
            "code": "MANIFEST_ID_MISMATCH",
            "message": "Client package manifest does not match the current server requirement",
            "retryable": True,
        }

        denied_again = client.send("EnterLobby", {"nickname": "仍未同步"})
        assert_error(client.receive(), denied_again, "PACKAGE_PROTOCOL_REQUIRED")

        ready_id = client.send("PackageReady", {"manifest_id": EMPTY_ID})
        assert client.expect("PackageReadyResult", ready_id)["data"] == {"ready": True}
        lobby_id = client.send("EnterLobby", {"nickname": "包同步玩家"})
        entered = client.expect("EnterLobbyResult", lobby_id)
        assert entered["data"]["user_id"] > 0
    finally:
        client.close()

    # Ready 只属于前一条 TCP Session；重连必须重新协商。
    reconnect = Client(port)
    try:
        reconnect.hello()
        denied = reconnect.send("EnterLobby", {"nickname": "重连玩家"})
        assert_error(reconnect.receive(), denied, "PACKAGE_PROTOCOL_REQUIRED")
    finally:
        reconnect.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: package_sync_integration_test.py SERVER_EXECUTABLE")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="sgs-package-sync-") as package_root:
        environment = os.environ.copy()
        environment["SGS_BIND_ADDRESS"] = "127.0.0.1"
        environment["SGS_PORT"] = str(port)
        environment["SGS_PACKAGE_ROOT"] = package_root
        server = subprocess.Popen(
            [sys.argv[1]], env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        try:
            wait_for_server(port)
            run_scenario(port)
            server.send_signal(signal.SIGTERM)
            return_code = server.wait(timeout=5)
            stdout, stderr = server.communicate()
            assert return_code == 0, (return_code, stdout, stderr)
            assert "started successfully" in stdout
            assert "Stopping CardGameServer." in stdout
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=2)


if __name__ == "__main__":
    main()
