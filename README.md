# 玩家档案切换 · PlayerProfiles

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

将 `bin/PlayerProfiles/` 放入服务端 `plugins/`。配置路径为 `plugins/PlayerProfiles/config/config.json`，参考 [config.example.json](config.example.json)。API 地址、API Key 和超时均通过配置读取，凭据不写入源码。服务端必须开启 `online-mode=true`。

首次安装或替换 DLL 后需要重启服务器。玩家日常切换档案只需重新加入，服务器无需重启。插件不执行启动时的全档案扫描，也不会重新启用旧配置中的 `scan_on_start` 或 `apply_on_start`。

接口使用 `POST <base_url>/openapi/helper/get_user_details`，请求体包含 `uid`，`Authorization` 头使用配置的原始 API Key。只接受返回 UID 与请求一致且签名完全匹配的成功响应；仅忽略签名末尾换行。

## 持久数据

`plugins/PlayerProfiles/data/` 中保存：

- `profiles.leveldb/`：由 LL `ll::data::KeyValueDB` 管理的档案登记数据库。完整登记以 CBOR 二进制编码，通过 `WriteBatch` 原子提交。
- `profiles.commit.cbor`：带版本、提交代数与 SHA-256 的二进制提交检查点。LL 数据库接口没有同步刷盘开关，因此先刷盘记录提交决定，启动时重放未完成的数据库写入。
- `originals/<档案编号>/`：网易原始记录和身份记录的首次备份，以及校验信息。
- `switches/<切换编号>/`：切换前身份索引、当前档案最后保存的数据和切换记录。

验证码和请求令牌只存在内存中，关闭验证表单或断线后即失效。档案内容继续存在世界存储中，由游戏存储接口读写，插件不直接打开 LevelDB。

不要单独删除数据库目录或提交检查点。备份世界时应同时备份插件的完整 `data/`。切换失败时保留所有数据并阻止该账号继续登录；世界索引已写入但登记提交尚未完成的切换，会在下一次档案读取前识别并完成。

本项目用于新存档与全新登记，不导入旧版插件数据。安装时使用独立的 `PlayerProfiles` 目录，并停用旧插件，避免重复注册 `/profile`。

## LeviLamina 接口与目录约定

配置通过 `NativeMod::getConfigDir()` 定位，持久登记和备份通过 `getDataDir()` 定位，不在源码中拼接插件安装目录。当前服务器安装下分别对应 `plugins/PlayerProfiles/config/` 和 `plugins/PlayerProfiles/data/`。

档案登记使用 LL 的 `KeyValueDB`。写入前校验 UID 独占归属、身份别名、当前档案及切换任务的关联；数据库锁排除其他写入实例，登记只在服务器线程修改。世界档案通过 `LevelStorage` 的读取、批量写入和刷盘接口操作。当前实现按单个世界使用，更换世界时需同时处理插件数据。

生命周期通过 `LL_REGISTER_MOD` 注册：`load()` 读取配置与登记，`enable()` 注册指令、事件和钩子，`disable()` 停止工作线程并移除事件、作废表单状态。钩子在禁用后仅调用游戏原逻辑；由于钩子和已提交的表单回调仍可能引用 DLL，`unload()` 明确拒绝动态卸载，更新插件需正常停服。

指令、表单、事件、日志、主线程调度和钩子使用 LL API。游戏没有提供完整的档案切换公共接口，因此读取前和退出后的时机使用 BDS 函数钩子；这部分依赖固定的游戏与 LL 版本，不能视为跨版本通用接口。

目录与数据接口参考 [LL Mod 文档](https://lamina.levimc.org/api_reference/mod/) 和 [LL Data 文档](https://lamina.levimc.org/api_reference/data/)。

## 编译和验证

需要 Windows x64、Visual Studio 2022 C++ 工具链、LLVM 22（clang-cl）、xmake，以及对应版本的 LeviLamina SDK。依赖由 xmake 安装。

C++ 源码与测试使用仓库中的 `.clang-format`，四空格缩进、最多 120 列；函数、条件分支和循环展开，避免将多条语句挤在一行。修改后可运行 `clang-format -i <文件>`，检查使用 `clang-format --dry-run --Werror <文件>`。

```powershell
xmake f -y -p windows -a x64 -m release --vs=2022
xmake build PlayerProfiles
xmake build core-tests
xmake run core-tests
xmake build PlayerProfilesFixture
```

基础测试覆盖 UID 编解码、验证码格式、网易 NBT 清理、归属独占和非法切换。`PlayerProfilesFixture` 是单独的测试 DLL，在名为 `profiles-test` 的空测试世界中执行 `profiletest`，验证真实世界存储写入、原始记录保留、切换往返、独立进度、幂等恢复及重启持久性；`profiledbtest` 使用真实 LL 数据库验证独占归属、身份冲突、非法写入拒绝、数据库锁、重新打开和中断提交恢复。测试 DLL 不随正式插件安装，拒绝在其他世界或有玩家在线时执行。

服务端测试不能代替真实 MC 客户端验证；表单显示、实际断线重连和游戏内背包恢复仍需客户端操作确认。网易 `scriptData`、`saveLongString` 只在可用副本中清理，原始记录保留。网易脚本功能本身不在迁移范围内。

Skip32 算法来源及许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

未知格式或损坏的数据库与检查点会拒绝加载，不自动修复或初始化为空。备份建议正常停服后同时保存世界及插件 `data/`。登记提交只覆盖插件元数据，世界索引切换仍通过备份和持久任务进行中断恢复。

## 仓库与发布

这是独立插件项目，初始工程结构来自 LeviLamina 模组模板；保留其 CC0 许可证和第三方声明，提交历史从本项目开始。

GitHub Actions 构建服务端插件与测试 DLL，检查 C++ 格式并运行 `core-tests`。真实数据库和世界存储测试需在独立 BDS 测试服运行。发布版本使用 `v<版本号>` Git 标签生成包版本；未打标签的开发构建可能显示为 `0.0.0`。发布附件名为 `PlayerProfiles-windows-x64.zip`，只包含插件产物、示例配置和文档，不包含 API 凭据、数据库或世界存档。

`tooth.json` 暂按 `Liliya233/PlayerProfiles` 配置发布地址；如果实际创建在其他账号或组织下，发布前应修改该地址。仓库尚未连接远程时，此配置不会创建或发布 GitHub 仓库。

许可证与依赖来源见 [LICENSE](LICENSE) 和 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
