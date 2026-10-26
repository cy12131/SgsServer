# M3-A-S 里程碑状态报告

状态：**服务端资源包管理、确定性清单和 Package Sync v1 已实现；自动化验收结果见下文。**

## 基线与契约

- 正式目录：`/home/cy/Code/SgsServerRepo`
- 开发分支：`feature/m3-a-server-packages`
- 稳定基线：`v0.3.0-m2`，peeled commit `34efd19e3e6e47e73b6e1accc76276cbcde4564c`
- 架构契约：SgsArchitecture `5b78b419a98a4fa18d68873444b695960cbfa066`
- 读取并实现：Package Manifest v1、Package Sync v1、M3 Agent Interface Matrix、对应 Schema、示例、RFC 8785 向量及 ADR-0007/0008
- 技术栈保持 C++20、CMake、CTest、Boost.Asio/Boost.JSON；新增运行时外部工具仅为现有 Git CLI

## 实现摘要

`PackageManager` 独立于 LobbyService，负责公开 HTTPS/SSH Git 仓库注册、完整 commit 锁定、Manifest 原始字节 SHA-256、Schema 等价校验、API 兼容检查、依赖版本/缺失/循环/重复检测、稳定拓扑顺序、启停事务和注册表持久化。Git 通过 fork/exec 参数数组调用，不经过 shell；禁用仓库 hook、交互提示、用户/系统配置和 `ext` 协议，生产禁用 `file` 协议及 URL 内嵌凭据。

安装先进入同状态目录 staging；下载、checkout 和所有验证成功后才 rename 至 `installed/<id>/<commit>`。审核修订后，注册表读操作使用共享跨进程锁，install/enable/disable/set-enabled 使用覆盖“锁内重读—验证—修改—持久化”的排他锁。注册表使用 PID+原子计数命名、`O_EXCL` 创建的临时文件加原子 rename，避免并发冲突和旧快照覆盖。启用先构造并编码完整候选清单，任何依赖、持久化或 65536 字节正文限制错误都不会替换旧有效清单。启动/reload 会重新核对 HEAD、Manifest 与注册摘要。

`manifest_id` 对按包 ID 排序、依赖 ID 排序的 Package Sync 七字段数组执行 RFC 8785 JCS 和 SHA-256。实现通过架构仓库的 RFC 8785 第 3.2.2/3.2.3、空清单及多包摘要向量。`manifest_sha256` 单独表示 commit 中 `manifest.json` 原始字节摘要。

`PackageSyncService` 为每个数值 Session ID 保存独立确认摘要。连接默认 NotReady；空清单仍需 PackageReady；断线和清单变化清除确认。GetPackageManifest、PackageReady、PackageReadyResult 和 PackageManifestChanged 保持 SGS/1 四字段信封及原长度帧。EnterLobby/CreateRoom/JoinRoom 增加 `PACKAGE_PROTOCOL_REQUIRED` 门禁，Hello/Ping/Echo 保持兼容。

## 管理与运行

新增 `sgs-package-admin`，提供 `list`、`install URL COMMIT`、`enable ID`、`disable ID` 和 `set-enabled ID...`。状态目录由 `SGS_PACKAGE_ROOT` 指定，服务端默认回退到 XDG/HOME state 路径。CLI 与服务端是独立进程；M3-A 不实现文件监视，外部变更后正在运行的服务端必须在维护窗口正常重启才能载入新清单。进程内切换接口会立即使 Session NotReady 并广播 PackageManifestChanged。

## 自动化验收

可复现命令：

```bash
cmake -S . -B build-m3-validation -DCMAKE_BUILD_TYPE=Debug
cmake --build build-m3-validation -j2
ctest --test-dir build-m3-validation --output-on-failure
```

测试覆盖：真实本地 Git 仓库安装及 commit 锁定；缺失/非法 Manifest；路径与符号链接边界；下载失败回滚；依赖缺失、版本、循环和重复 ID；确定性加载顺序；注册表重载与摘要复核；两个真实 `sgs-package-admin` 进程并发安装、启用、停用且不丢失更新；架构标准摘要向量；候选清单超限；空清单同步、错误摘要、正确 Ready、变更失效和断线清理；真实 TCP Hello/Ping/Echo、Package Sync 门禁及重连；原 M1/M2 网络与大厅房间回归。

实测结果：

- Debug 配置与 `cmake --build ... -j2` 成功，无编译警告。
- 正式目录审核修订版 Debug CTest：6/6 通过；新增 `package_admin_concurrency`，并保留 `application_smoke`、`package_manager`、`network_integration`、`lobby_room_integration`、`package_sync_integration`，总计约 19.28 秒。
- ASan + UBSan 独立构建成功；同一组 CTest 6/6 通过，总计约 25.34 秒，未报告 sanitizer 错误。
- M2 压力回归暴露并修复了 Session 关闭与已完成异步写回调之间的队列生命周期竞态；对应 `lobby_room_integration` 随后连续运行 3 次均通过。
- 公开仓库负向验证：从 `https://github.com/cy12131/SgsArchitecture.git` 精确 checkout 架构 commit 成功，随后按契约以 `PACKAGE_MANIFEST_MISSING` 拒绝；未生成 `registry.json`，旧配置不受影响。

## 兼容性与已知限制

- M2 客户端仍可 Hello/Ping/Echo，但若不实现 Package Sync，EnterLobby 会收到 `PACKAGE_PROTOCOL_REQUIRED`；这是 ADR-0008 规定的有意协议变化。
- 架构仓库在指定提交中没有提供可直接安装的独立公开合法测试包仓库 URL；合法安装验收使用真实 Git 命令操作的受控本地仓库，生产 URL 规则另有单元校验。公开 SgsArchitecture 仓库可用于验证“根目录缺少 manifest.json”失败路径。
- 私有仓库凭据、在线注册表监视、资源包热加载、Lua/GameProcessManager、规则/卡牌/技能、Windows 客户端和实际对局不在 M3-A-S 范围。
- Git 下载尚无总超时；仓库大小、checkout 文件数、单文件大小、临时目录与安装目录总磁盘用量等资源配额尚未实现。部署时只应使用可信、规模受控的仓库并提供外部监控。
- M3-A 不强制驱逐清单变化前已经在房间中的用户；只使其包同步状态失效。对局中的版本隔离与安全热切换留待后续统一设计。

M3-A-S 完成后停止，不合并 main、不创建发布标签、不进入 Lua 或游戏规则阶段。
