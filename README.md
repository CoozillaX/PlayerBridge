# PlayerBridge

PlayerBridge 是一个基于 LeviLamina 的 Minecraft 基岩版服务端插件，让国际服玩家找回并使用网易导出存档中的角色，也支持在多个角色档案之间切换。

玩家通过修改网易账号签名验证身份，找回该账号在当前世界中的角色。原有国际服角色也会保留，可以随时切回。每份网易档案只能由一个国际服账号持有。

## 使用

1. 输入 `/profile` 打开菜单，选择“找回网易角色”。
2. 输入网易 UID，将网易签名改为表单显示的 6 位验证码，再点击“完成”。
3. 验证成功后，在“我的档案”中选择要使用的角色。
4. 确认切换后会断开连接，重新加入服务器即可使用所选角色。

关闭验证表单后验证码失效。切换无需重启服务器，各角色的游戏进度分别保存。

## 安装

支持 **Windows x64 · BDS 1.26.51.1 · LeviLamina 26.51.6**。

1. 正常停服，将构建产物 `bin/PlayerBridge/` 放入服务端 `plugins/`。
2. 按 [配置示例](config.example.json) 设置 `plugins/PlayerBridge/config/config.json`，填写 `api.base_url` 和 `api.api_key`。
3. 确保 `server.properties` 中 `online-mode=true`，启动服务器。

验证接口需要支持 `POST /openapi/helper/get_user_details`，返回对应 UID 的签名；API Key 通过 `Authorization` 请求头传递。

角色数据保存在世界中，插件只登记归属和切换状态。首次找回会清理原网易档案中的专有字段；之后通过切换国际服账号的档案索引使用原档案，不创建角色副本。

备份时请正常停服，同时保存世界和 `plugins/PlayerBridge/data/`。当前版本使用全新登记，不导入旧版插件数据；更换世界时也需对应更换插件数据。

## 编译

需要 Visual Studio 2022 C++ 工具链、LLVM 22 和 xmake。

```powershell
xmake f -y -p windows -a x64 -m release --vs=2022
xmake build PlayerBridge
```

核心测试：`xmake build core-tests` 后运行 `xmake run core-tests`。服务端测试插件 `PlayerBridgeFixture` 仅用于独立测试服，不随正式插件安装。

[许可证](LICENSE) · [第三方声明](THIRD_PARTY_NOTICES.md)
