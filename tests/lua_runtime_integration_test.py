#!/usr/bin/env python3
"""M3-B2 Runtime contract tests using real pipes and the real Lua interpreter."""

import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def frame(message):
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode()
    return struct.pack(">I", len(body)) + body


def read_exact(descriptor, size):
    result = bytearray()
    while len(result) < size:
        part = os.read(descriptor, size - len(result))
        if not part:
            raise RuntimeError("unexpected Runtime EOF")
        result.extend(part)
    return bytes(result)


def read_message(descriptor):
    size = struct.unpack(">I", read_exact(descriptor, 4))[0]
    return json.loads(read_exact(descriptor, size))


def bindings(root, player_count):
    package = {
        "id": "org.sgs.runtime-test",
        "version": "1.0.0",
        "commit": "1" * 40,
        "root": str(root),
        "server_entry": "server.lua",
    }
    players = [
        {"player_id": f"player-{index}", "seat": index, "display_name": f"玩家{index}"}
        for index in range(1, player_count + 1)
    ]
    return package, players


def launch(runtime, root, player_count=2, entry="server.lua"):
    package, players = bindings(root, player_count)
    package["server_entry"] = entry
    parent_to_child_read, parent_to_child_write = os.pipe()
    child_to_parent_read, child_to_parent_write = os.pipe()
    arguments = [
        runtime,
        "--game-id", "game-runtime-1",
        "--room-id", "42",
        "--manifest-id", "a" * 64,
        "--mode-id", "standard",
        "--ipc-read", str(parent_to_child_read),
        "--ipc-write", str(child_to_parent_write),
        "--package", json.dumps(package, separators=(",", ":")),
    ]
    for player in players:
        arguments += ["--player", json.dumps(player, ensure_ascii=False, separators=(",", ":"))]
    process = subprocess.Popen(
        arguments,
        pass_fds=(parent_to_child_read, child_to_parent_write),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    os.close(parent_to_child_read)
    os.close(child_to_parent_write)
    return process, parent_to_child_write, child_to_parent_read, package, players


def init_message(package, players, **changes):
    message = {
        "ipc_version": "1",
        "type": "InitGame",
        "game_id": "game-runtime-1",
        "sequence": "1",
        "request_token": "init_runtime_0001",
        "payload": {
            "room_id": "42",
            "manifest_id": "a" * 64,
            "mode_id": "standard",
            "seed": "1234",
            "players": players,
            "packages": [{"id": package["id"], "commit": package["commit"]}],
            "settings": {},
        },
    }
    for key, value in changes.items():
        if key == "game_id":
            message["game_id"] = value
        else:
            message["payload"][key] = value
    return message


def finish(process, write_descriptor, read_descriptor):
    os.close(write_descriptor)
    os.close(read_descriptor)
    output = process.communicate(timeout=5)[0]
    return process.returncode, output


def test_ready(runtime, root, player_count):
    process, writer, reader, package, players = launch(runtime, root, player_count)
    os.write(writer, frame(init_message(package, players)))
    ready = read_message(reader)
    assert ready["type"] == "GameReady"
    assert ready["sequence"] == "1"
    assert ready["request_token"] == "init_runtime_0001"
    assert ready["payload"]["runtime_api_version"] == "1.0-draft"
    shutdown = {
        "ipc_version": "1", "type": "Shutdown", "game_id": "game-runtime-1",
        "sequence": "2", "payload": {"reason": "test complete"},
    }
    os.write(writer, frame(shutdown))
    code, output = finish(process, writer, reader)
    assert code == 0, output


def test_coalesced_init_and_shutdown(runtime, root):
    process, writer, reader, package, players = launch(runtime, root)
    shutdown = {
        "ipc_version": "1", "type": "Shutdown", "game_id": "game-runtime-1",
        "sequence": "2", "payload": {"reason": "coalesced test"},
    }
    os.write(writer, frame(init_message(package, players)) + frame(shutdown))
    ready = read_message(reader)
    assert ready["type"] == "GameReady"
    code, output = finish(process, writer, reader)
    assert code == 0, output


def test_binding_mismatch(runtime, root, field, value):
    process, writer, reader, package, players = launch(runtime, root)
    os.write(writer, frame(init_message(package, players, **{field: value})))
    failed = read_message(reader)
    assert failed["type"] == "GameFailed"
    assert failed["payload"]["error"]["code"] == "LUA_RUNTIME_INIT_FAILED"
    code, output = finish(process, writer, reader)
    assert code != 0 and b"initialization failed" in output


def test_invalid_initial_sequence(runtime, root):
    process, writer, reader, package, players = launch(runtime, root)
    message = init_message(package, players)
    message["sequence"] = "2"
    os.write(writer, frame(message))
    failed = read_message(reader)
    assert failed["type"] == "GameFailed" and failed["sequence"] == "1"
    code, _ = finish(process, writer, reader)
    assert code != 0


def test_entry_escape(runtime, root, outside):
    link = root / "escape.lua"
    link.symlink_to(outside)
    process, writer, reader, _, _ = launch(runtime, root, entry="escape.lua")
    failed = read_message(reader)
    assert failed["type"] == "GameFailed"
    code, _ = finish(process, writer, reader)
    assert code != 0


def test_package_root_symlink(runtime, real_root, temporary):
    alias = temporary / "package-alias"
    alias.symlink_to(real_root, target_is_directory=True)
    process, writer, reader, _, _ = launch(runtime, alias)
    failed = read_message(reader)
    assert failed["type"] == "GameFailed"
    code, _ = finish(process, writer, reader)
    assert code != 0


def main():
    if len(sys.argv) != 2:
        raise RuntimeError("expected sgs-lua-runtime path")
    runtime = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="sgs-lua-runtime-") as temporary:
        root = pathlib.Path(temporary) / "package"
        root.mkdir()
        (root / "server.lua").write_text(
            "local p=Runtime.registerPackage({id='org.sgs.runtime-test',version='1.0.0'})\n"
            "p:registerGameMode({id='standard',min_players=2,max_players=8,"
            "create_logic=function(ctx) local v={}; table.insert(v,ctx.player_count); "
            "assert(v[1]>=2); return {} end})\n",
            encoding="utf-8",
        )
        test_ready(runtime, root, 2)
        test_ready(runtime, root, 8)
        test_coalesced_init_and_shutdown(runtime, root)
        test_binding_mismatch(runtime, root, "game_id", "game-wrong")
        test_binding_mismatch(runtime, root, "room_id", "99")
        test_binding_mismatch(runtime, root, "manifest_id", "b" * 64)
        test_invalid_initial_sequence(runtime, root)
        outside = pathlib.Path(temporary) / "outside.lua"
        outside.write_text("return {}\n", encoding="utf-8")
        test_entry_escape(runtime, root, outside)
        test_package_root_symlink(runtime, root, pathlib.Path(temporary))
    print("Lua Runtime integration tests passed")


if __name__ == "__main__":
    main()
