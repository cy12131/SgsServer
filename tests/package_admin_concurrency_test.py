#!/usr/bin/env python3
"""验证两个真实 sgs-package-admin 进程不会发生注册表丢失更新。"""

import fcntl
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def run(arguments):
    return subprocess.run(arguments, check=True, text=True, capture_output=True)


def make_checkout(root, package_id):
    repository = root / (package_id + "-source")
    repository.mkdir()
    (repository / "server").mkdir()
    manifest = json.dumps({
        "schema_version": 1,
        "id": package_id,
        "name": package_id,
        "version": "1.0.0",
        "api_version": "1.0",
        "dependencies": [],
        "server": {"entry": "server/init.lua"},
    }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    (repository / "manifest.json").write_bytes(manifest)
    (repository / "server/init.lua").write_text("return {}\n", encoding="utf-8")
    run(["git", "-C", str(repository), "init", "-b", "main"])
    run(["git", "-C", str(repository), "config", "user.name", "SGS Test"])
    run(["git", "-C", str(repository), "config", "user.email", "test@invalid"])
    run(["git", "-C", str(repository), "add", "."])
    run(["git", "-C", str(repository), "commit", "-m", "fixture"])
    commit = run(["git", "-C", str(repository), "rev-parse", "HEAD"]).stdout.strip()
    digest = hashlib.sha256(manifest).hexdigest()
    return repository, commit, digest


def write_registry(state, entries, enabled_ids):
    document = {
        "schema_version": 1,
        "packages": [dict(entry, enabled=entry["id"] in enabled_ids) for entry in entries],
    }
    (state / "registry.json").write_text(
        json.dumps(document, separators=(",", ":")), encoding="utf-8"
    )


def launch(admin, state, *arguments, environment=None):
    return subprocess.Popen(
        [admin, "--root", str(state), *arguments],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def wait_success(process):
    stdout, stderr = process.communicate(timeout=10)
    assert process.returncode == 0, (process.returncode, stdout, stderr)


def enabled_ids(state):
    document = json.loads((state / "registry.json").read_text(encoding="utf-8"))
    return {entry["id"] for entry in document["packages"] if entry["enabled"]}


def installed_ids(state):
    document = json.loads((state / "registry.json").read_text(encoding="utf-8"))
    return {entry["id"] for entry in document["packages"]}


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: package_admin_concurrency_test.py ADMIN_EXECUTABLE")
    admin = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix="sgs-package-admin-race-") as temporary:
        root = pathlib.Path(temporary)
        state = root / "state"
        state.mkdir()
        first = make_checkout(root, "org.sgs.concurrent_a")
        second = make_checkout(root, "org.sgs.concurrent_b")

        # Git 仍走 ssh:// 生产 URL；受控测试传输器只在子进程中把 upload-pack
        # 定向到本地夹具，不为正式管理程序开放 file:// 协议。
        ssh_wrapper = root / "test-ssh.py"
        ssh_wrapper.write_text(
            "#!/usr/bin/env python3\n"
            "import os, shlex, sys\n"
            "command = shlex.split(sys.argv[-1])\n"
            "assert len(command) == 2 and command[0] == 'git-upload-pack'\n"
            "os.execvp(command[0], command)\n",
            encoding="utf-8",
        )
        ssh_wrapper.chmod(0o700)
        git_environment = os.environ.copy()
        git_environment["GIT_SSH"] = str(ssh_wrapper)
        git_environment["GIT_SSH_VARIANT"] = "ssh"

        install_processes = [
            launch(
                admin, state, "install", f"ssh://example.invalid{first[0]}", first[1],
                environment=git_environment,
            ),
            launch(
                admin, state, "install", f"ssh://example.invalid{second[0]}", second[1],
                environment=git_environment,
            ),
        ]
        for process in install_processes:
            wait_success(process)
        assert installed_ids(state) == {
            "org.sgs.concurrent_a", "org.sgs.concurrent_b"
        }
        entries = json.loads(
            (state / "registry.json").read_text(encoding="utf-8")
        )["packages"]

        # 两个进程都可能在构造阶段读取旧快照；变更方法必须在取得排他锁后重读。
        for _ in range(5):
            write_registry(state, entries, set())
            processes = [
                launch(admin, state, "enable", "org.sgs.concurrent_a"),
                launch(admin, state, "enable", "org.sgs.concurrent_b"),
            ]
            for process in processes:
                wait_success(process)
            assert enabled_ids(state) == {
                "org.sgs.concurrent_a", "org.sgs.concurrent_b"
            }

        # 相反方向的并发修改也必须基于锁内最新状态组合，而非最后写入者覆盖。
        write_registry(state, entries, {"org.sgs.concurrent_a"})
        processes = [
            launch(admin, state, "disable", "org.sgs.concurrent_a"),
            launch(admin, state, "enable", "org.sgs.concurrent_b"),
        ]
        for process in processes:
            wait_success(process)
        assert enabled_ids(state) == {"org.sgs.concurrent_b"}

        # 外部持锁期间管理程序必须等待，证明锁覆盖读取和计算，而不只是 rename。
        lock_path = state / ".registry.lock"
        with lock_path.open("a+b") as lock_file:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            blocked = launch(admin, state, "enable", "org.sgs.concurrent_a")
            time.sleep(0.2)
            assert blocked.poll() is None, "administrator did not wait for transaction lock"
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
            wait_success(blocked)

        assert not list(state.glob("registry.json.tmp.*")), "temporary registry leaked"
        run([admin, "--root", str(state), "list"])
        print("Concurrent package administrator tests passed.")


if __name__ == "__main__":
    main()
