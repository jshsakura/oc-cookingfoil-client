<div align="center">
  <h1><img src="brand/cookingfoil-readme-256.png" alt="CookingFoil" width="192" height="192" /></h1>
  <p><strong>ファイルは自分のサーバーに。ライブラリは本体に。</strong></p>
  <p>ゲーム、自作アプリ、セーブ、パッチをまとめて扱う個人用の自作クライアントです。</p>
  <p>
  <a href="https://jshsakura.github.io/oc-cookingfoil/?lang=ja"><img src="brand/buttons/website-ja.svg" alt="ウェブサイト" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.ja.md#クライアントの接続"><img src="brand/buttons/connect-ja.svg" alt="本体を接続" width="160" height="44" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil/blob/main/README.ja.md#クイックスタート"><img src="brand/buttons/setup-ja.svg" alt="設置ガイド" width="160" height="44" /></a>
  <a href="docs/README.md"><img src="brand/buttons/docs-ja.svg" alt="ドキュメント" width="160" height="44" /></a>
  </p>
  <p><a href="README.md">English</a> · <a href="README.ko.md">한국어</a> · <a href="README.ja.md">日本語</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.es.md">Español</a></p>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml"><img src="https://github.com/jshsakura/oc-cookingfoil-client/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/jshsakura/oc-cookingfoil-client/releases"><img src="https://img.shields.io/github/v/release/jshsakura/oc-cookingfoil-client?color=FFC23D" alt="GitHub release" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-16161A" alt="GPL-3.0-or-later" /></a>
</div>

![CookingFoil のホーム画面](docs/screenshots/en/01-home.png)

自分で立てた [CookingFoil サーバー](https://github.com/jshsakura/oc-cookingfoil)に接続し、ファイルを見ながら
インストールするものを選べます。複数のサーバーをまとめて見たり、インストール済みのバージョンを更新と比べたり、
途中で止まったダウンロードを続けたりできます。

## 画面

| ゲームの詳細 | ダウンロード |
|---|---|
| ![ゲームの詳細](docs/screenshots/en/02-detail.png) | ![ダウンロード](docs/screenshots/en/04-downloads.png) |
| インストール済みのゲーム | セーブと Dropbox |
| ![インストール済みのゲーム](docs/screenshots/en/07-installed.png) | ![セーブと Dropbox](docs/screenshots/en/08-dropbox-linked.png) |

サンプルデータを使って英語の実際の画面を撮影しています。アイコンのない項目は、アプリが描く頭文字のタイルで表示されます。

## 欲しいものを選ぶだけ。あとはまとめて整えます

| やりたいこと | CookingFoil がすること |
|---|---|
| **探してインストールする** | 検索、絞り込み、並べ替え、ゲームの詳細。本編、更新、DLC を選び、SD カードか本体の保存先を選べます。NSP、NSZ、XCI、XCZ は CyberFoil エンジンでインストールします。 |
| **複数のサーバーを使う** | カタログをまとめて表示し、同じものが複数のサーバーにあればダウンロード元を選べます。 |
| **ダウンロードを続ける** | キューと、完了・失敗・キャンセルの記録を残します。対応するダウンロードは止まった位置から再開し、接続が戻るのを待ち、その間は本体をスリープさせません。 |
| **更新を探す** | インストール済みのゲームをサーバーのバージョンと比べます。自作アプリの更新はサーバーが渡すハッシュで確かめます。 |
| **自作アプリを管理する** | NRO 単体やアプリのフォルダーをインストール・更新します。対応するアプリは HOME メニューにショートカットを作れます。 |
| **セーブを守る** | ユーザーごとのセーブを本体内でバックアップ・復元し、サーバーのセーブを取り込み、Dropbox にアップロード・ダウンロードできます。復元の前には元に戻すためのバックアップを作ります。 |
| **パッチを当てる** | ゲームファイル、チート、IPS、FPSLocker のパッチを有効・無効にします。ファイルの衝突を確かめ、チートはビルド ID ごとにまとめます。 |
| **レトロゲームを遊ぶ** | 機種ごとにゲームを選び、セットでダウンロードします。サーバーが用意する RetroArch パック、ショートカット、RetroAchievements の情報を使います。 |
| **空きを作る** | インストール済みのゲームを管理し、確認のうえで残りかすを片付けます。片付けるのは、どのインストール済みタイトルも使っていないものだけです。 |

画面の言語は韓国語、英語、日本語、中国語で、タッチとコントローラーの両方で操作できます。自作アプリ、パッチ、レトロ、
一部の連携機能はサーバーが対応しているときに表示されます。セーブとパッチをいつ適用するかは自分で決めます。

## はじめかた

1. **サーバーを用意します。** [CookingFoil サーバーの案内](https://github.com/jshsakura/oc-cookingfoil/blob/main/README.ja.md)に沿って自分で立てるか、使う許可のあるサーバーを用意します。
2. **クライアントを入れます。** 最新リリースの [`cookingfoil.nro`](https://github.com/jshsakura/oc-cookingfoil-client/releases/latest/download/cookingfoil.nro) か、サーバーのウェブページから受け取ったものを、SD カードの `switch/cookingfoil/` に置きます。設定ファイルは要りません。
3. **接続します。** ホームブリューメニューからアプリを開き、**設定 → サーバーを追加**でアドレスを入れます。PC で作った `config.json` を置いてもかまいません。
4. **本体を承認します。** 承認が必要なサーバーでは本体に QR コードが出ます。スマートフォンでログインして承認すると本体がつながります。

ホームブリューを動かせる本体が必要です。セーブの管理など本体の権限が要る操作は、アプリケーションモードで行います。
アプレットモードで使えない場合は、起動のしかたをアプリが案内します。

設定、認証、複数のサーバーについては[はじめかた (英語)](docs/GETTING-STARTED.md)を見てください。

## 開発とプレビュー

C++17 と SDL2 のホスト版は、本物の画面を PC で動かします。サンプルデータを見たりサーバーにつないだりしながら、
ブラウザーから操作できます。

```sh
git clone --recursive https://github.com/jshsakura/oc-cookingfoil-client
cd oc-cookingfoil-client
# 先にビルド案内にあるホスト用の依存パッケージを入れます。
make test host
./build-host/cookingfoil-host --fixture host/fixtures --lang ja --serve 38520
```

`http://localhost:38520/` を開きます。本体へのインストール、セーブへのアクセス、削除は実機で確かめます。
依存パッケージ、本体用のビルド、スクリーンショットは[ビルドとプレビュー (英語)](docs/BUILDING.md)にあります。

## もっと読む

| 文書 | 内容 |
|---|---|
| [はじめかた (英語)](docs/GETTING-STARTED.md) | インストール、設定、QR 承認、複数のサーバー、SD カードのファイル |
| [機能 (英語)](docs/FEATURES.md) | ダウンロード、自作アプリ、セーブ、パッチ、レトロの動きと条件 |
| [ビルドとプレビュー (英語)](docs/BUILDING.md) | ホスト版と本体用のビルド、テスト、ブラウザーでのプレビュー |
| [デザインのルール (英語)](docs/DESIGN.md) | 画面の構成、フォーカス、入力と反応 |
| [文言と翻訳 (英語)](docs/WRITING.md) | 英語の翻訳キー、4 言語の文言と書きかた |
| [作業のルール (英語)](docs/CONTRIBUTING-GUIDE.md) | スレッド、安全なファイル書き込み、テスト、プルリクエスト |
| [サードパーティのコード (英語)](third_party/README.md) | インストールエンジンとパッチの管理 |

## ライセンスとコンテンツ

クライアントは [GPL-3.0-or-later](LICENSE)、別プロジェクトのサーバーは MIT ライセンスです。
クライアントは Awoo Installer と Tinfoil から派生した [CyberFoil](https://github.com/luketanti/CyberFoil) の
インストールエンジンを使います。詳しくは[サードパーティの表示 (英語)](THIRD_PARTY_NOTICES.md)を見てください。

ゲーム、キー、BIOS、RetroArch は含みません。正当に所有しているか使用の許可があるコンテンツと、
配布が認められたパッチだけを使ってください。
