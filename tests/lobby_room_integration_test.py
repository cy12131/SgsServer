#!/usr/bin/env python3
"""M2 用户、大厅和房间的真实 SGS/1 TCP 集成测试。"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time


def encode(message):
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
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
    def __init__(self, port, name="Client"):
        self.connection = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.connection.settimeout(3)
        self.next_id = 1
        self.name = name

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def send(self, kind, data, request_id=None):
        if request_id is None:
            request_id = self.next_id
            self.next_id += 1
        self.connection.sendall(encode({
            "protocol": 1, "type": kind, "request_id": request_id, "data": data
        }))
        return request_id

    def receive(self):
        length = struct.unpack("!I", receive_exact(self.connection, 4))[0]
        return json.loads(receive_exact(self.connection, length).decode("utf-8"))

    def expect(self, kind, request_id=None):
        message = self.receive()
        assert message["type"] == kind, (self.name, kind, message)
        if request_id is not None:
            assert message["request_id"] == request_id, (self.name, request_id, message)
        return message

    def expect_error(self, request_id, code):
        message = self.expect("Error", request_id)
        assert message["data"]["code"] == code, (self.name, code, message)
        return message

    def hello(self):
        request_id = self.send("Hello", {
            "client_name": self.name, "client_version": "0.1.0-dev"
        })
        return self.expect("HelloAck", request_id)

    def enter(self, nickname):
        request_id = self.send("EnterLobby", {"nickname": nickname})
        return self.expect("EnterLobbyResult", request_id)


def wait_for_server(port):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            client = Client(port, "Probe")
            client.close()
            return
        except OSError:
            time.sleep(0.02)
    raise AssertionError("server did not listen within five seconds")


def player(snapshot, user_id):
    return next(item for item in snapshot["data"]["players"] if item["user_id"] == user_id)


def create_and_snapshot(client, name, maximum):
    request_id = client.send("CreateRoom", {"name": name, "max_players": maximum})
    result = client.expect("CreateRoomResult", request_id)
    snapshot = client.expect("RoomSnapshot", 0)
    assert snapshot["data"]["room_id"] == result["data"]["room_id"]
    return result["data"]["room_id"], snapshot


def run_primary_scenario(port):
    owner = Client(port, "Owner")
    guest = Client(port, "Guest")
    watcher = Client(port, "Watcher")
    no_lobby = Client(port, "NoLobby")
    clients = [owner, guest, watcher, no_lobby]
    try:
        for client in clients:
            client.hello()

        # M1 回归：已握手连接仍支持 Ping 和中文 Echo。
        ping_id = owner.send("Ping", {})
        owner.expect("Pong", ping_id)
        echo_id = owner.send("Echo", {"text": "M2 仍支持中文 Echo"})
        assert owner.expect("EchoResult", echo_id)["data"]["text"] == "M2 仍支持中文 Echo"

        first = owner.enter("同名玩家")
        second = guest.enter("同名玩家")
        watcher_user = watcher.enter("  大厅观察者  ")
        owner_id = first["data"]["user_id"]
        guest_id = second["data"]["user_id"]
        assert owner_id != guest_id
        assert watcher_user["data"]["nickname"] == "大厅观察者"

        duplicate_id = owner.send("EnterLobby", {"nickname": "另一个名字"})
        owner.expect_error(duplicate_id, "ALREADY_IN_LOBBY")
        denied_id = no_lobby.send("CreateRoom", {"name": "X", "max_players": 2})
        no_lobby.expect_error(denied_id, "LOBBY_REQUIRED")

        room_id, created = create_and_snapshot(owner, "两人房", 2)
        assert created["data"]["revision"] == 1
        assert player(created, owner_id) == {
            "user_id": owner_id, "nickname": "同名玩家", "seat": 1,
            "ready": False, "is_owner": True,
        }
        guest.expect("RoomListChanged", 0)
        watcher.expect("RoomListChanged", 0)

        list_id = guest.send("ListRooms", {})
        listing = guest.expect("ListRoomsResult", list_id)
        assert listing["data"]["rooms"] == [{
            "room_id": room_id, "name": "两人房", "owner_nickname": "同名玩家",
            "player_count": 1, "max_players": 2, "status": "waiting",
        }]

        join_id = guest.send("JoinRoom", {"room_id": room_id})
        guest.expect("JoinRoomResult", join_id)
        guest_joined = guest.expect("RoomSnapshot", 0)
        owner_joined = owner.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        assert guest_joined == owner_joined
        assert guest_joined["data"]["revision"] == 2
        assert player(guest_joined, guest_id)["seat"] == 2

        full_id = watcher.send("JoinRoom", {"room_id": room_id})
        watcher.expect_error(full_id, "ROOM_FULL")
        repeat_id = owner.send("JoinRoom", {"room_id": room_id})
        owner.expect_error(repeat_id, "ALREADY_IN_ROOM")
        owner_ready_id = owner.send("SetReady", {"ready": True})
        owner.expect_error(owner_ready_id, "PERMISSION_DENIED")

        ready_id = guest.send("SetReady", {"ready": True})
        guest.expect("SetReadyResult", ready_id)
        ready_guest = guest.expect("RoomSnapshot", 0)
        ready_owner = owner.expect("RoomSnapshot", 0)
        assert ready_guest == ready_owner
        assert ready_guest["data"]["revision"] == 3
        assert player(ready_guest, guest_id)["ready"] is True

        invalid_id = guest.send("SetReady", {"ready": "yes"})
        guest.expect_error(invalid_id, "INVALID_MESSAGE")
        unchanged_id = guest.send("SetReady", {"ready": True})
        guest.expect("SetReadyResult", unchanged_id)
        unchanged = guest.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        assert unchanged["data"]["revision"] == 3

        not_ready_id = guest.send("SetReady", {"ready": False})
        guest.expect("SetReadyResult", not_ready_id)
        left_ready = guest.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        assert left_ready["data"]["revision"] == 4

        leave_id = guest.send("LeaveRoom", {})
        guest.expect("LeaveRoomResult", leave_id)
        guest.expect("RoomListChanged", 0)
        owner_after_leave = owner.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        assert owner_after_leave["data"]["revision"] == 5

        rejoin_id = guest.send("JoinRoom", {"room_id": room_id})
        guest.expect("JoinRoomResult", rejoin_id)
        rejoined = guest.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        assert player(rejoined, guest_id)["seat"] == 2
        assert rejoined["data"]["revision"] == 6

        ready_id = guest.send("SetReady", {"ready": True})
        guest.expect("SetReadyResult", ready_id)
        guest.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)

        owner_leave_id = owner.send("LeaveRoom", {})
        owner.expect("LeaveRoomResult", owner_leave_id)
        owner.expect("RoomListChanged", 0)
        promoted = guest.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        promoted_player = player(promoted, guest_id)
        assert promoted["data"]["owner_id"] == guest_id
        assert promoted_player["seat"] == 2
        assert promoted_player["ready"] is False
        assert promoted_player["is_owner"] is True

        final_leave_id = guest.send("LeaveRoom", {})
        guest.expect("LeaveRoomResult", final_leave_id)
        guest.expect("RoomListChanged", 0)
        owner.expect("RoomListChanged", 0)
        watcher.expect("RoomListChanged", 0)
        list_id = watcher.send("ListRooms", {})
        assert watcher.expect("ListRoomsResult", list_id)["data"]["rooms"] == []

        not_room_id = watcher.send("LeaveRoom", {})
        watcher.expect_error(not_room_id, "NOT_IN_ROOM")
        missing_id = watcher.send("JoinRoom", {"room_id": 999999})
        watcher.expect_error(missing_id, "ROOM_NOT_FOUND")

        # 容量 8 的合法边界。
        room8, snapshot8 = create_and_snapshot(owner, "八人房", 8)
        assert snapshot8["data"]["max_players"] == 8
        guest.expect("RoomListChanged", 0)
        watcher.expect("RoomListChanged", 0)
        leave8 = owner.send("LeaveRoom", {})
        owner.expect("LeaveRoomResult", leave8)
        owner.expect("RoomListChanged", 0)
        guest.expect("RoomListChanged", 0)
        watcher.expect("RoomListChanged", 0)
        assert room8 > room_id

        # 非法房间输入不改变房间列表。
        for data in (
            {"name": "", "max_players": 2},
            {"name": "房" * 41, "max_players": 2},
            {"name": "小", "max_players": 1},
            {"name": "大", "max_players": 9},
        ):
            invalid_room_id = owner.send("CreateRoom", data)
            owner.expect_error(invalid_room_id, "INVALID_ROOM_DATA")
        list_id = owner.send("ListRooms", {})
        assert owner.expect("ListRoomsResult", list_id)["data"]["rooms"] == []

        # 异常断线触发房主转移；最后一名成员断线删除房间。
        abrupt_room, _ = create_and_snapshot(owner, "断线房", 2)
        guest.expect("RoomListChanged", 0)
        watcher.expect("RoomListChanged", 0)
        join_id = guest.send("JoinRoom", {"room_id": abrupt_room})
        guest.expect("JoinRoomResult", join_id)
        guest.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        owner.close()
        promoted = guest.expect("RoomSnapshot", 0)
        watcher.expect("RoomListChanged", 0)
        assert promoted["data"]["owner_id"] == guest_id
        guest.close()
        watcher.expect("RoomListChanged", 0)
        list_id = watcher.send("ListRooms", {})
        assert watcher.expect("ListRoomsResult", list_id)["data"]["rooms"] == []
    finally:
        for client in clients:
            client.close()


def run_input_and_seat_scenario(port):
    # request_id=0 仅保留给服务端通知；客户端业务请求按 SGS/1 公共字段错误关闭。
    zero_id = Client(port, "ZeroRequest")
    zero_id.hello()
    zero_id.send("EnterLobby", {"nickname": "零"}, request_id=0)
    assert zero_id.connection.recv(1) == b""
    zero_id.close()

    # 昵称 Unicode 字符数、控制字符和空白校验。
    for nickname in ("   ", "名" * 21, "坏\u0001名字"):
        client = Client(port, "InvalidNickname")
        try:
            client.hello()
            request_id = client.send("EnterLobby", {"nickname": nickname})
            client.expect_error(request_id, "INVALID_NICKNAME")
        finally:
            client.close()

    owner = Client(port, "SeatOwner")
    first = Client(port, "SeatFirst")
    second = Client(port, "SeatSecond")
    replacement = Client(port, "SeatReplacement")
    clients = [owner, first, second, replacement]
    try:
        identities = {}
        for client in clients:
            client.hello()
            identities[client.name] = client.enter(client.name)["data"]["user_id"]

        room_id, _ = create_and_snapshot(owner, "座位测试", 3)
        first.expect("RoomListChanged", 0)
        second.expect("RoomListChanged", 0)
        replacement.expect("RoomListChanged", 0)

        join = first.send("JoinRoom", {"room_id": room_id})
        first.expect("JoinRoomResult", join)
        first.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        second.expect("RoomListChanged", 0)
        replacement.expect("RoomListChanged", 0)

        join = second.send("JoinRoom", {"room_id": room_id})
        second.expect("JoinRoomResult", join)
        snapshot = second.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        first.expect("RoomSnapshot", 0)
        replacement.expect("RoomListChanged", 0)
        assert player(snapshot, identities["SeatSecond"])["seat"] == 3

        leave = first.send("LeaveRoom", {})
        first.expect("LeaveRoomResult", leave)
        first.expect("RoomListChanged", 0)
        owner.expect("RoomSnapshot", 0)
        second.expect("RoomSnapshot", 0)
        replacement.expect("RoomListChanged", 0)

        join = replacement.send("JoinRoom", {"room_id": room_id})
        replacement.expect("JoinRoomResult", join)
        snapshot = replacement.expect("RoomSnapshot", 0)
        owner.expect("RoomSnapshot", 0)
        second_snapshot = second.expect("RoomSnapshot", 0)
        first.expect("RoomListChanged", 0)
        assert player(snapshot, identities["SeatReplacement"])["seat"] == 2
        assert player(second_snapshot, identities["SeatSecond"])["seat"] == 3

        # 快速交替准备，revision 每次实际变化都递增。
        revisions = []
        for ready in (True, False, True, False, True, False):
            request_id = second.send("SetReady", {"ready": ready})
            second.expect("SetReadyResult", request_id)
            changed = second.expect("RoomSnapshot", 0)
            owner.expect("RoomSnapshot", 0)
            replacement.expect("RoomSnapshot", 0)
            revisions.append(changed["data"]["revision"])
        assert all(after == before + 1 for before, after in zip(revisions, revisions[1:]))
    finally:
        for client in clients:
            client.close()


def run_resource_limit_scenario(port):
    owners = []
    try:
        for index in range(100):
            client = Client(port, f"Limit{index}")
            client.hello()
            client.enter(f"限制{index}")
            create_and_snapshot(client, f"资源房{index}", 2)
            owners.append(client)

        overflow = Client(port, "Overflow")
        overflow.hello()
        overflow.enter("超限用户")
        request_id = overflow.send("CreateRoom", {"name": "第101间", "max_players": 2})
        overflow.expect_error(request_id, "RESOURCE_LIMIT")
        overflow.close()
    finally:
        # 此时没有留在大厅的用户，逐个断开不会产生待消费的大厅通知。
        for client in owners:
            client.close()


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lobby_room_integration_test.py SERVER_EXECUTABLE")

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    environment = os.environ.copy()
    environment["SGS_BIND_ADDRESS"] = "127.0.0.1"
    environment["SGS_PORT"] = str(port)
    server = subprocess.Popen(
        [sys.argv[1]], env=environment, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True,
    )
    try:
        wait_for_server(port)
        run_primary_scenario(port)
        run_input_and_seat_scenario(port)
        run_resource_limit_scenario(port)
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
