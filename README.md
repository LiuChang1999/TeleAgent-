# TeleAgent 账号工作台 v0.7.0

[下载 Windows 64 位单文件 EXE](https://github.com/LiuChang1999/TeleAgent-/releases/download/v0.7.0/TeleAgent-Account-Desk-v0.7.0-win64.exe) · [下载分发 ZIP](https://github.com/LiuChang1999/TeleAgent-/releases/download/v0.7.0/TeleAgent-Account-Desk-v0.7.0-win64.zip) · [Release 说明](https://github.com/LiuChang1999/TeleAgent-/releases/tag/v0.7.0) · [使用说明](docs/使用说明-v0.7.0.txt)

C++20 桌面程序，面向本机 TeleAgent 2.7.1 的账号目录与登录状态测试。新版界面使用系统已有的 Microsoft Edge WebView2，HTML/CSS/JavaScript 已嵌入 EXE；文件读取、身份匹配、切换及备份操作由 C++ 执行。

## 运行

双击 **TeleAgentAccounts.exe**。v0.7.0 已将微软签名的 WebView2Loader.dll 嵌入 EXE，启动时自动释放到当前用户管理器的 runtime 缓存并核对内容，无需旁置 DLL。WebView2 Runtime 仍需在目标电脑上可用。

- 自动识别 TeleAgent 的系统安装记录、运行进程、常见目录及开始菜单快捷方式，支持 TeleClaw 安装目录；也可点击 **程序路径** 手动选择 `TeleAgent.exe`，选择会按当前 Windows 用户保存。
- **新登录账号**：先保存当前账号会话，备份设备登录入口，再打开 TeleAgent 供本人登录另一个账号。已有账号目录、令牌、聊天记录和各自浏览器分区保留，登录完成后账号卡片自动刷新。
- 每次切换或准备新登录前，先验证 TeleAgent 程序存在；找不到程序时不会先修改账号选择。

- 每 3 秒刷新用户名、手机号、备注和已保存会话。应用独占 Cookie 文件时显示最近保存的到期信息；切换时自动同步快照，不再把占用误判成登录资料缺失。
- 点击“切换并打开”后自动保存当前会话、关闭 TeleAgent、选择目标账号并重新打开，再通过应用启动记录和账号 ID 核对身份，无须手动退出。
- 点击账号卡片的铅笔或详情中的“修改备注”，可保存最多 64 个字符的备注；清空即可移除。备注只保存在管理器中，不改 TeleAgent 的用户名。
- 手机号仅显示缓存已有的内容；打码维持原样，没有缓存就显示“未缓存”。
- 历史分区可能缓存其他账号的身份。展示信息按缓存里的账号 ID 匹配，并标注历史来源；分区身份不一致时阻止直接复用登录。
- Cookie 未到期不代表服务端仍接受该会话。管理器不进行远程验证，由“切换并打开”后的 TeleAgent 实际验证。

## 托盘运行

点击“收起到托盘”、最小化或关闭按钮后，工作台在后台继续刷新。悬停托盘图标显示当前账号备注或名称、应用状态、账号总数和已保存数量；执行账号操作时显示当前进度。单击图标恢复窗口，右键可打开或退出；再次运行程序会唤回已有窗口。退出不会关闭 TeleAgent。

## 操作

1. 在账号卡片中选择已保存的目标账号。
2. 点击 **切换并打开**，等待进度完成；关闭、保存和重启均由程序自动完成。
3. 每次切换保留当前账号的登录快照，并通过 TeleAgent 启动后的身份记录检查目标账号。没有保存过登录的账号，需要先登录一次。
4. **重新登录**：自动关闭应用，备份并清理所选账号的本机登录状态，再打开 TeleAgent 供你手动登录。聊天数据库和配置保留。
5. **本机退出登录**：备份后移出所选账号的浏览器分区、令牌和本地密钥，并设置重新登录标记。不会请求服务端注销。服务端注销应使用 TeleAgent 自带的退出登录。
6. 在 **会话备份** 页面恢复最近一次操作。若已经产生新的登录资料，恢复会停止，避免覆盖。

程序启动和自动刷新不修改 TeleAgent 源数据。切换会先请求应用正常关闭，再结束经路径和进程创建时间核对的 TeleAgent 残留进程；若检测到正在执行的任务，会保留当前会话并暂停切换。

默认读取 %USERPROFILE%\.local\share\TeleAgent。配置目录 %USERPROFILE%\.config\TeleAgent 保留原处，不参与移动。

新版自身数据保存在 `%LOCALAPPDATA%\TeleAgentAccountDesk\manager-data`，按 Windows 用户分别保存，不依赖解压位置是否可写。临时读取快照随后清理；操作备份在其 `backups`，账号会话快照在 `sessions`，备注在 `account-notes.json`。该目录限制为当前 Windows 用户及 SYSTEM 访问。旧版程序旁的 `manager-data` 原样保留；发现后可在“会话备份”页打开旧版目录。旧备份属于原数据目录，不能套用到其他 Windows 用户。

分发给其他用户时，可发送 `dist\TeleAgent账号工作台-v0.7.0-单文件.exe`，或 `dist\TeleAgent账号工作台-v0.7.0-win64-single.zip`。压缩包采用固定文件清单，仅包含程序、许可证和使用说明，不包含任何账号、备注、Cookie、令牌、会话或备份。对方需要在本机安装 TeleAgent，并自行完成账号登录；通常不需要管理员权限。WebView2 Runtime 仍须在目标电脑上可用。

账号切换依赖 TeleAgent 的内部格式，尚非官方接口。升级 TeleAgent 后应重新检查兼容性。账号目录存在、凭据文件存在、本地 Cookie 未到期都不能单独证明服务端登录有效。程序不会恢复服务端已注销或失效的凭据。

## 编译与验证

### 本地新增功能（源码编译）

- “设置中心”可调整系统托盘、关闭窗口行为、最小化到托盘、手动启动时收起、开机自启及自启时收起、系统通知、账号刷新间隔（手动/3/5/10/30 秒）。保存后生效，支持先恢复默认再保存。
- 关闭托盘会同时关闭依赖托盘的隐藏行为，关闭窗口将退出；任务操作期间会保留窗口。底部始终提供“退出工作台”。
- 开机自启默认关闭，仅在保存启用后登记当前用户 Windows Run 启动项；读取状态、构建、测试不会启用真实自启。启动项使用当前 EXE 的完整引用路径；移动程序后重新保存可更新路径。Windows 自身的“启动应用”设置也可能影响是否启动。

- “轮询登录”对当前筛选范围执行一轮账号切换、启动身份核对、本地凭据检查和快照保存；失败写入逐项结果与操作历史，并提供窗口和托盘提示。支持停止；任务运行、无法关闭或保存失败会中止。正常结束后尝试回到原账号。失效登录需本人手动重新登录，不能依靠本地记录保证服务端有效。结果在 `last-poll.json`。
- 单文件 EXE 已内嵌 WebView2 加载库和许可证；程序底部可查看许可证。加载库自动释放到受保护的本机缓存，内容损坏时修复。

- 账号支持按备注、用户名、ID、已有手机号搜索；在详情的“分组与置顶”中创建或选择分组，留空为未分组。卡片星标可快速置顶，列表支持分组和仅置顶筛选。
- “操作历史”展示最近 200 条切换、新登录、重新登录、本机退出、恢复与备份清理记录，可搜索并按结果筛选；兼容已有切换历史。新登录/重新登录的成功表示已打开登录入口，并非服务端已确认登录。
- “会话备份”可扫描快照和操作备份的空间占用，按账号汇总，预览旧备份清理范围。支持保留最近 1、3、5、10 份；始终保护当前快照、最近恢复所需备份及未完成或来源异常的资料。清理需要确认，备份发生变化时必须重新扫描；链接或目录重定向会阻止清理。清理不操作 TeleAgent 的源账号数据。
- 右键托盘中的“切换账号”可直接切换；置顶账号优先，其次为最近成功切换的账号。当前账号、缺少登录资料或身份不一致的账号不可直接选择；切换仍执行任务检查和身份核对。

分组和置顶保存在管理器的 `account-organization.json`，新增历史在 `operation-history.json`。新版单文件可执行文件生成在 `build-local\v0.7.0\TeleAgentAccounts.exe`；若旧程序正在运行，可使用 `.\build.ps1 -SkipRootCopy` 先编译，退出旧工作台后再运行新程序。

本机可用 GCC / MinGW-w64、CMake 和 Ninja：

```powershell
.\build.ps1
.\TeleAgentAccounts.exe --self-test tests
.\TeleAgentAccounts.exe --control-test tests
.\TeleAgentAccounts.exe --path-test tests
.\TeleAgentAccounts.exe --tray-test tests
.\TeleAgentAccounts.exe --desk-test tests
.\TeleAgentAccounts.exe --poll-test tests
.\TeleAgentAccounts.exe --loader-test tests
.\TeleAgentAccounts.exe --ui-test tests
.\TeleAgentAccounts.exe --settings-test tests
```

自测结果在 tests\selftest-results.json，测试使用新建的模拟目录，不切换真实账号。覆盖 JSON、LevelDB 日志与 SST / Snappy / CRC32C、删除记录重放、身份错位、Cookie WAL 快照、账号切换、备份恢复、新登录覆盖拦截及运行进程保护。

路径测试另覆盖带空格和中文的自定义目录、注册表图标路径、失效路径跳过、非法 EXE 拦截，以及手动路径保存。`--path-report <JSON文件>` 可只读输出当前安装识别结果。可用 `--teleagent-exe <完整路径>` 临时指定启动文件；自动控制在缺少程序时先停止，不修改账号数据。

一次性只读导出（仅账号展示字段与 Cookie 元数据，不导出 Cookie 值或令牌）：

```powershell
.\TeleAgentAccounts.exe --report local-account-report.json
```

模拟目录调试：

```powershell
.\TeleAgentAccounts.exe --data-root "模拟数据目录" --state-root "模拟管理器目录"
```

--data-root 会启用模拟模式并禁止启动真实 TeleAgent。

SQLite 使用 Windows 系统 winsqlite3.dll，只打开程序私有快照，不会用 SQLite 打开源数据库。LevelDB 按活动 MANIFEST、序列号与删除记录读取，校验 CRC32C，支持未压缩和 Snappy 块；遇到不支持的格式显示读取失败。

自动关闭使用 Windows 应用关闭消息，不修改 TeleAgent 安装包、不启用被应用禁用的调试接口，也不调用服务端退出登录。应用升级后需重新检查启动记录的兼容性。

## 依赖来源

WebView2 SDK / 加载库：Microsoft.Web.WebView2 **1.0.3800.47**，来自微软官方 NuGet 包，来源和 SHA256 记录在 third_party\webview2\SOURCE.txt，许可证在同目录。加载库带 Microsoft Corporation 的有效签名，位于 `third_party\webview2\x64`，构建时嵌入 EXE，SHA256 同样记录在 SOURCE.txt。界面不加载外部页面、字体或脚本；嵌入页面的网络连接被 CSP 禁止，消息仅接受嵌入页面的来源。

单文件构建与分发：

```powershell
.\build.ps1 -SkipRootCopy
.\package.ps1
```

测试可直接运行 `build-local\v0.7.0\TeleAgentAccounts.exe` 的上述自测参数，目录中无需 DLL。打包脚本默认使用此目录中的新版本，不会误用仍在运行的根目录旧 EXE。

窗口与后台设置保存在 `desktop-settings.json`，与原有程序路径 `settings.json` 分开。设置自测仅修改隔离测试注册表项，不改真实 Run 启动项。
