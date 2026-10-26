# M3-B2 Lua Runtime（第一部分）

`sgs-lua-runtime` 是每局独立启动的 Linux 子进程。父进程仍由
`GameProcessManager` 管理，FD 3 为父到子 GameIPC/1，FD 4 为子到父
GameIPC/1；stdout/stderr 合并到第三条非阻塞管道，由父进程持续排空。
启动不经过 shell，环境只保留受控的 `PATH`、`LANG` 和 `TZ`。

父进程从 `PackageManager::freeze_generation()` 取得服务器启动时冻结的
generation。每个 `--package` 参数都是 JSON，明确携带包 ID、版本、锁定
commit、canonical owner root 和相对 `server.entry`；每个 `--player` 参数
同样明确携带玩家 ID、座位和展示名。Runtime 不扫描共享 packages 根目录，
也不读取 registry、`.staging` 或 Git 分支。入口必须是所属物理包根内的普通
文件，符号链接或路径逃逸会在 fork 前或 Runtime 初始化时被拒绝。

Runtime 使用系统 Lua 5.4 运行库；CMake 优先采用匹配的 Lua 5.4 开发头，
仅在当前系统缺少开发包时退回项目内部的最小 C ABI 声明。只打开 Lua
base 与纯内存 `table` 库，不开放 `package`、`io`、`os`、`debug`，不继承
`LUA_PATH` / `LUA_CPATH`，并移除 `dofile` 和 `loadfile`。包按 generation 顺序加载，并通过
`Runtime.registerPackage{...}` 与 `pkg:registerGameMode{...}` 注册自身和模式。
InitGame 必须逐项匹配 CLI 中冻结的 game、room、manifest、packages 和
roster；模式人数范围限定在框架的 2–8 人内。创建最小 Game 上下文成功后，
Runtime 回送关联 init token 的 GameReady，并在收到 sequence 2 的 Shutdown
后正常退出。本阶段不处理 PlayerAction、PlayerRequest 或 PlayerReply。

日志通道使用固定 16 KiB 读缓冲，不缓存原始日志。默认每条最多计数 4096
字节、每秒最多产生 32 条安全摘要；超额记录只累计丢弃计数，超长行累计截断
计数。原始 Lua 输出（包括潜在私有数据）不进入主服务日志回调。日志 EOF、
读取错误或子进程崩溃不阻断 GameIPC 回收。

当前边界：Lua 表尚未提供完整只读代理、确定性 RNG、CPU/内存/FD 沙箱、
Request 协程桥接、规则事件系统和房间开始游戏接入。这些能力必须在后续
M3-B2/B3 工作中补齐；本阶段实现不能被视为完整游戏规则沙箱。
