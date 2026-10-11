# 玩家档案切换 · NeteasePlayerMigration

面向 Windows x64、BDS 1.26.51.1 与 LeviLamina 26.51.6 的服务端插件。国际服玩家验证网易签名后，可将导出世界中的网易角色档案找回到自己的列表，并在档案之间切换。

## 游戏内使用

1. 输入 `/profile` 打开菜单。“我的档案”包含首次登记时使用的初始档案。
2. 选择“找回网易档案”，输入网易 UID。
3. 将网易签名改为表单中的 6 位数字，点击“完成”。关闭表单即作废验证码；下次生成新码，无验证码列表。
4. 验证成功后，找回当前世界中该 UID 的档案，并在“我的档案”中选择使用。
5. 在列表中选择档案并确认切换。插件断开玩家连接，等待旧会话保存退出；玩家手动重新加入后，新连接在 loadPlayerData 读取前应用待切换索引，再加载所选档案。旧会话未退出时不应用切换并阻止进入游戏。

初始档案保持独立；网易原始记录不会删除或覆盖。首次找回网易档案时，使用 LL 存储接口生成独立可用副本，后续进度由服务器正常保存到对应副本。切换只修改当前国际服身份索引及其别名的 `ServerId`，不会把某个档案内容反复覆盖到另一个档案。归属使用认证 XUID；存档定位使用加载钩子提供的 `MsaId` 和 `SelfSignedId`，不假定 MsaId 等于数字 XUID。

查找只按网易 UID 对应的存档身份键定位，必要时沿该键的 ServerId 读取角色数据。跨版本遗留的其他角色索引不会列为独立档案。已经被旧脚本删除的原始记录不会凭空恢复。

每个网易 UID 及其角色档案只能归属一个国际服 XUID；同一个国际服账号可以拥有多份档案。归属在添加副本前持久登记，避免并发验证和重启造成重复授权。默认不提供释放、转让或删除档案的功能。

## 安装和配置

将 `bin/NeteasePlayerMigration/` 放入服务端 `plugins/`。配置路径为 `plugins/NeteasePlayerMigration/config/config.json`，参考 [config.example.json](config.example.json)。API 地址、API Key 和超时均通过配置读取，凭据不写入源码。服务端必须开启 `online-mode=true`。

首次安装或替换 DLL 后需要重启服务器。玩家日常切换档案只需重新加入，服务器无需重启。插件不执行启动时的全档案扫描，也不会重新启用旧配置中的 `scan_on_start` 或 `apply_on_start`。

接口使用 `POST <base_url>/openapi/helper/get_user_details`，请求体包含 `uid`，`Authorization` 头使用配置的原始 API Key。只接受返回 UID 与请求一致且签名完全匹配的成功响应；仅忽略签名末尾换行。

## 持久数据

`plugins/NeteasePlayerMigration/data/` 中保存：

- `profiles.sqlite3`：账号、档案、网易 UID 独占归属、身份别名和待切换任务。SQLite 唯一约束与外键校验归属，状态更新通过事务提交。
- `profiles.json.pre-sqlite.bak`：首次升级时保留的旧登记文件。旧 `profiles.json` 仅用于首次导入，导入成功后不再作为登记依据，也不继续更新。
- `originals/<档案编号>/`：网易原始记录和身份记录的首次备份，以及校验信息。
- `switches/<切换编号>/`：切换前身份索引、当前档案最后保存的数据和切换记录。

验证码和请求令牌只存在内存中，关闭验证表单或断线后即失效。档案内容继续存在世界存储中，由游戏存储接口读写，插件不直接打开 LevelDB。

不要单独删除 `profiles.sqlite3`：它是唯一归属和档案索引的依据。备份世界时应同时备份插件的 `data/`。切换失败时会保留所有数据并阻止该账号继续登录，供管理员检查；世界索引已写入但登记事务尚未完成的切换会在下一次账号档案读取前识别并完成。

旧版非空 `tasks.json` 不会被静默忽略，需要管理员先转换旧迁移记录。

## 编译和验证

需要 Windows x64、Visual Studio 2022 C++ 工具链、LLVM 22（clang-cl）、xmake，以及对应版本的 LeviLamina SDK。SQLite 静态链接，依赖由 xmake 安装。

```powershell
xmake f -y -p windows -a x64 -m release --vs=2022
xmake build NeteasePlayerMigration
xmake build core-tests
xmake run core-tests
xmake run registry-tests
```

基础测试覆盖 UID 编解码、验证码格式、网易 NBT 清理、归属独占、非法切换和登记重载。`registry-tests` 覆盖 SQLite 导入、唯一归属约束、事务回滚、过期写入拒绝和重启持久性。`NpmStorageFixture` 是单独的测试 DLL，可在名为 `migration-profile-test` 的空测试世界中通过控制台 `npmtest` 验证 LL 真实存储写入、原始记录保留、切换往返、独立进度、幂等恢复及重启持久性。它不会随正式插件安装，拒绝在其他世界或有玩家在线时执行。

服务端测试不能代替真实 MC 客户端验证；表单显示、实际断线重连和游戏内背包恢复仍需客户端操作确认。网易 `scriptData`、`saveLongString` 只在可用副本中清理，原始记录保留。网易脚本功能本身不在迁移范围内。

Skip32 算法来源及许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

数据库首次导入保留所有初始档案、找回档案、当前选择与待切换记录；损坏或未知版本的数据库会拒绝加载，不会自动使用陈旧 JSON 覆盖。数据库使用完整刷盘和回滚日志，备份建议正常停服后同时保存世界及插件 data 目录。SQLite 事务只能覆盖插件登记数据，世界索引切换仍通过备份和持久任务进行中断恢复。

## 仓库与发布

这是独立插件项目，初始工程结构来自 LeviLamina 模组模板；保留其 CC0 许可证和第三方声明，提交历史从本项目开始。

GitHub Actions 仅构建服务端插件，并运行 `core-tests` 和 `registry-tests`。发布版本使用 `v<版本号>` Git 标签生成包版本；未打标签的开发构建可能显示为 `0.0.0`。发布附件名为 `NeteasePlayerMigration-windows-x64.zip`，只包含插件产物、示例配置和文档，不包含 API 凭据、数据库或世界存档。

`tooth.json` 暂按 `Liliya233/NeteasePlayerMigration` 配置发布地址；如果实际创建在其他账号或组织下，发布前应修改该地址。仓库尚未连接远程时，此配置不会创建或发布 GitHub 仓库。

许可证与依赖来源见 [LICENSE](LICENSE) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
