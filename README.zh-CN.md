<div align="center">
  <h1><img src="brand/cookingfoil-readme-256.png" alt="CookingFoil" width="192" height="192" /></h1>
  <p><strong>文件放在自己的服务器上，游戏库装在主机里。</strong></p>
  <p>一款个人自制客户端，用来管理游戏、自制应用、存档和补丁。</p>
  <p>
  <a href="https://jshsakura.github.io/oc-cookingfoil/?lang=zh"><img src="brand/buttons/website-zh.svg" alt="网站" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.zh-CN.md#连接客户端"><img src="brand/buttons/connect-zh.svg" alt="连接主机" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.zh-CN.md#快速开始"><img src="brand/buttons/setup-zh.svg" alt="安装指南" width="160" height="44" /></a>
  <a href="docs/README.md"><img src="brand/buttons/docs-zh.svg" alt="使用文档" width="160" height="44" /></a>
  </p>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.es.md">Español</a></p>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml"><img src="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/releases"><img src="https://img.shields.io/github/v/release/jshsakura/oc-cookingfoil-client?color=FFC23D" alt="GitHub release" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-16161A" alt="GPL-3.0-or-later" /></a>
</div>

![CookingFoil 主页画面](docs/screenshots/en/01-home.png)

连接你自己搭建的 [CookingFoil 服务器](https://github.com/jshsakura/oc-cookingfoil)，浏览文件，选择要安装的内容。
可以同时浏览多台服务器，把已安装的版本和更新对比，还能继续中断的下载。

## 看一看

| 游戏详情 | 下载 |
|---|---|
| ![游戏详情](docs/screenshots/en/02-detail.png) | ![下载](docs/screenshots/en/04-downloads.png) |
| 已安装的游戏 | 存档与 Dropbox |
| ![已安装的游戏](docs/screenshots/en/07-installed.png) | ![存档与 Dropbox](docs/screenshots/en/08-dropbox-linked.png) |

截图来自真实的英文界面，使用示例数据。没有图标的项目会显示应用自带的首字母方块。

## 选你想要的，其余交给它整理

| 你想做的事 | CookingFoil 负责的部分 |
|---|---|
| **浏览并安装** | 搜索、筛选、排序和游戏详情。可以选择本体、更新和 DLC 文件，以及装到 SD 卡还是主机存储。NSP、NSZ、XCI 和 XCZ 通过 CyberFoil 引擎安装。 |
| **使用多台服务器** | 合并显示各服务器的目录；同一内容在多台服务器上都有时，可以选择从哪台下载。 |
| **继续下载** | 保留下载队列，以及已完成、失败和已取消的记录。支持的下载会从保存的位置继续，等待网络恢复，并在下载期间让主机保持唤醒。 |
| **查找更新** | 把已安装的游戏和服务器上的版本对比。自制应用的更新使用服务器提供的内容哈希校验。 |
| **管理自制应用** | 安装和更新单个 NRO 或整个应用文件夹。支持的应用可以在 HOME 菜单创建快捷方式。 |
| **保护存档** | 在本机按用户备份和还原存档，导入服务器上的存档，并上传或下载 Dropbox 备份。还原前会先做一份可以回退的备份。 |
| **应用补丁** | 启用或停用游戏文件、金手指、IPS 和 FPSLocker 补丁。检查文件冲突，并按 Build ID 合并金手指。 |
| **玩复古游戏** | 按机种挑选游戏并下载整套文件。使用服务器提供的 RetroArch 包、快捷方式和 RetroAchievements 信息。 |
| **腾出空间** | 管理已安装的游戏，确认后清理残留文件。只清理没有任何已安装游戏使用的内容。 |

界面支持韩语、英语、日语和中文，可用触屏和手柄操作。自制应用、补丁、复古游戏和部分联动功能会根据服务器的支持情况显示。
存档和补丁何时应用由你自己决定。

## 开始使用

1. **准备服务器。** 按照 [CookingFoil 服务器说明](https://github.com/jshsakura/oc-cookingfoil/blob/main/README.zh-CN.md)自行搭建，或使用你有权访问的服务器。
2. **放入客户端。** 从最新版本下载 [`cookingfoil.nro`](https://github.com/jshsakura/oc-cookingfoil-client/releases/latest/download/cookingfoil.nro)，或从服务器网页下载，然后复制到 SD 卡的 `switch/cookingfoil/` 文件夹。不需要配置文件。
3. **连接服务器。** 从自制软件菜单打开应用，在 **设置 → 添加服务器** 中输入地址。也可以在电脑上写好 `config.json` 放进去。
4. **批准主机。** 如果服务器要求批准，主机上会显示二维码。用手机登录并批准后，主机就会连上。

需要一台能运行自制软件的主机。存档管理等需要主机权限的操作，请在应用模式下进行。
如果在小程序模式下无法访问，应用会说明如何启动。

配置、认证和多台服务器的用法见[开始使用（英文）](docs/GETTING-STARTED.md)。

## 开发与预览

基于 C++17 和 SDL2 的主机版会在电脑上运行真实界面。可以浏览示例数据或连接服务器，并在浏览器里操作。

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
# 先安装构建说明里列出的主机依赖。
make test host
./build-host/cookingfoil-host --fixture host/fixtures --lang zh --serve 38520
```

打开 `http://localhost:38520/`。安装到主机、访问存档和删除需要在实机上确认。
依赖、主机版构建和截图见[构建与预览（英文）](docs/BUILDING.md)。

## 更多文档

| 文档 | 内容 |
|---|---|
| [开始使用（英文）](docs/GETTING-STARTED.md) | 安装、配置、二维码批准、多台服务器和 SD 卡上的文件 |
| [功能（英文）](docs/FEATURES.md) | 下载、自制应用、存档、补丁、复古游戏的行为和条件 |
| [构建与预览（英文）](docs/BUILDING.md) | 主机版与 Switch 版构建、测试、浏览器预览 |
| [设计规则（英文）](docs/DESIGN.md) | 界面布局、焦点、输入与反馈 |
| [文案与翻译（英文）](docs/WRITING.md) | 英文翻译键、四种语言的文案与写作规则 |
| [工作规则（英文）](docs/CONTRIBUTING-GUIDE.md) | 线程、安全写文件、测试和拉取请求 |
| [第三方代码（英文）](third_party/README.md) | 安装引擎与补丁维护 |

## 许可证与内容

客户端采用 [GPL-3.0-or-later](LICENSE)，独立的服务器项目采用 MIT 许可证。
客户端使用 [CyberFoil](https://github.com/luketanti/CyberFoil) 的安装引擎，它源自 Awoo Installer 和 Tinfoil。
详见[第三方声明（英文）](THIRD_PARTY_NOTICES.md)。

本项目不包含游戏、密钥、BIOS 或 RetroArch。请只使用你合法拥有或获准使用的内容，以及允许分发的补丁。
