# Taiko Zucchini

> 🇬🇧 English? → **[English README here](README.md)**

Taiko Zucchini は、Namco System 357 上の太鼓の達人アーケード作品（カツ丼〜
グリーン）向けの PS3 SPRX MOD です。ランタイムプラグインとしてインストール
され、初回起動時のブートストラップ EBOOT を使ってコンソール上でゲーム本体の
オリジナル EBOOT をパッチします。

> ## 📖 まずこの README を最後までよく読んでください
> インストール失敗のほとんどは手順の飛ばし読みが原因です。何かに触れる前に
> すべて読んでください。特に **権限 (PERMISSIONS)** と **始める前に** の節は
> 必ず確認してください。

## 機能

| 機能 | 内容 |
|------|------|
| ドングル・VU パッチ | オリジナルのハードウェアなしで、アーケード USB ドングルと VU ストレージのチェックを通します。 |
| Chassisinfo リダイレクト | シャーシ情報を生成・リダイレクトし、太鼓の厄介な起動チェックの一つを通します。 |
| EBOOT セルフパッチ | 初回起動時にオリジナル EBOOT をパッチして再署名し、MOD が変わった際もパッチ済み EBOOT を最新に保ちます。 |
| 最新 HTTPS 対応 | ゲームの古いネットワーク経路の一部を mbedTLS ベースのクライアントに置き換え、最新の TLS エンドポイントに対応します。 |
| オンラインリダイレクトフック | プライベートサーバーへのルーティングや診断のための HTTP・DNS・ソケットフックを提供します。 |
| DualShock 対応 | コントローラ入力をゲームの USIO 形式の入力経路にマッピングします。 |
| キーボード対応 | ゲームプレイやオペレータ操作向けにキーボード入力を追加します。 |
| QR・カメラ対応 | バナパス風のカード処理向けにカメラフックと QR スキャンを追加します。 |
| MOD メニュー | よく使うランタイムオプションのゲーム内設定メニューを提供します。 |
| FTP サーバー | MOD メニューからオペレータ用 FTP サーバーを起動し、ファイル操作を容易にします。 |
| オーバーレイ通知 | CFW 専用の通知 API に頼らず、アップデート案内などのランタイムオーバーレイメッセージを表示します。 |

## 対応バージョン

Taiko Zucchini は **太鼓の達人 カツ丼** から **太鼓の達人 グリーン** までの
範囲に対応しています（カツ丼、ソライロ、モモイロ、キミドリ、ムラサキ、
ブルー／ホワイト、グリーン など）。

この範囲外の太鼓バージョンには、対応が明記されていない限りインストールしない
でください。パッチャーはバージョン固有の EBOOT レイアウトとパッチオフセットに
依存します。

### 複数バージョンの同時インストール

**複数の太鼓バージョンを並べてインストール**できます。それぞれが自身の
ゲームフォルダ（例：`SCEEXE001` のほか、グリーン・ホワイト・ムラサキ用など）
に入ります。各ゲームフォルダにブートストラップ EBOOT とその他のファイルが
揃っていれば、Zucchini が各バージョンを個別にパッチします。

唯一の条件は **すべてのゲームフォルダで権限が正しいこと**（権限の節を参照）。
これさえ守れば、いくつでも共存できます。

## 動作環境

レトロ PS3 本体では、Taiko Zucchini はカスタムファームウェア（CFW）が必要です。

オリジナルの Namco System 357 ハードウェアでは、ブートストラップと SPRX の
読み込みフローが動くように DEX ファームウェアモジュールのインストールが必要
です。

## 始める前に

### USB メモリは挿さない

MOD 実行中は **PS3 に USB メモリを挿さない**でください。空のものでもダメです。
USB メモリはドングル／VU 検出を混乱させる可能性があります。それ以外
（コントローラなど）は問題ありません。USB ストレージだけ外してください。

### RPCS3：USB 仮想ファイルシステムをクリーンにする

RPCS3 でも同じルールが適用され、加えてエミュレートされた USB デバイスを
クリーンにする必要があります。**Configuration → Virtual File System →
dev_usb** を開き、すべての Vendor ID・Product ID・Serial の項目を削除して、
各 `/dev_usbNNN` 行を空にしてください（`/dev_usb000` のみデフォルトのパスを
残します）。以下のようになります：

```text
Device        Path                          Vendor ID   Product ID   Serial
/dev_usb000   $(EmulatorDir)dev_usb000/
/dev_usb001
/dev_usb002
/dev_usb003
...
```

VID／PID／Serial がどれか設定されていると、**古い太鼓バージョンで必ず問題が
起きます。** 削除して **Save** をクリックしてください。

## リリース内容

通常のリリースには以下が含まれます：

- `zucchini.sprx`：ランタイムプラグイン。
- `EBOOT.BIN`：初回パッチに使う使い捨てのブートストラップ EBOOT。

リリースにはゲームファイル、Sony SDK ファイル、署名鍵、アプリローダ鍵、その他
の専有鍵素材は含まれません。

## 設定

設定は **グローバル** で、`zucchini.sprx` の隣に置かれます：

```text
/dev_hdd0/plugins/taiko/taiko_config.cfg
/dev_hdd0/plugins/taiko/cards.cfg
```

- `taiko_config.cfg`：メイン設定。すべてのゲームで共有されます。ゲームごとの
  オーバーレイで個別の値を上書きできますが、ベース設定はグローバルです。
- `cards.cfg`：カード登録データ。ここに保存されます。

両ファイルは存在しない場合、初回起動時に妥当なデフォルトで作成されます。

## ⚠️ 権限 (PERMISSIONS) — これを守らないと何も動きません ⚠️

> **`/dev_hdd0/plugins/taiko/` 以下、および各ゲームフォルダ
> `/dev_hdd0/game/<ゲームフォルダ>/` 以下の、すべてのファイルとフォルダを、
> 再帰的に `777` 権限・所有者 `root` に設定しなければなりません。**
>
> これが失敗の最も多い原因です。権限や所有者が誤っていると、MOD は設定の
> 読み書き、EBOOT のパッチ、あるいはロードに失敗します。多くの場合、
> 分かりづらいエラーや無言の失敗になります。

正しい作業順序：

1. **まずすべて**コピーする — ゲームと MOD のファイル両方。
2. すべてのファイルをセットアップする（設定、鍵、EBOOT のリネーム、
   DATA00000.BIN など）。
3. **その後、最後の手順として**、権限と所有者を再帰的に設定する。

権限・所有者の設定方法は 2 通り：

- **FTP** — CFW のレトロ本体、および HEN を導入した本体でのみ動作します。
  `chmod`／`chown` ができるアカウントで FTP クライアント（または内蔵 FTP
  サーバー、後述）を使い、`777` ＋ `root` を再帰的に適用します。
- **仮想マシン／HDD 直接編集** — PS3 の HDD を直接マウントしてそこで権限を
  修正します。S357 ハードウェアではこれが **唯一** の方法です。現状ネット
  ワーク越しに MOD を入れられないためです。

両方のツリーに再帰的に適用してください。例：

```sh
chmod -R 777 /dev_hdd0/plugins/taiko
chown -R root:root /dev_hdd0/plugins/taiko
chmod -R 777 /dev_hdd0/game/<ゲームフォルダ>
chown -R root:root /dev_hdd0/game/<ゲームフォルダ>
```

## インストール

1. PS3 のハードドライブに、このフォルダを作成します：

   ```text
   /dev_hdd0/plugins/taiko/
   ```

2. リリースの `zucchini.sprx` をそのフォルダにコピーします：

   ```text
   /dev_hdd0/plugins/taiko/zucchini.sprx
   ```

3. 同じフォルダに `keys` フォルダを作成します：

   ```text
   /dev_hdd0/plugins/taiko/keys/
   ```

4. 自分の署名鍵ファイルを `keys` フォルダにコピーします。パッチャーは
   ファイル名が `keys` と `ldr_curves` であることを期待します：

   ```text
   /dev_hdd0/plugins/taiko/keys/keys
   /dev_hdd0/plugins/taiko/keys/ldr_curves
   ```

   これらは MOD がオリジナル EBOOT を復号・パッチ・再署名するために必要です。
   Taiko Zucchini には同梱されません。

5. 太鼓グリーンのゲーム `USRDIR` フォルダを開きます。

6. オリジナルの、未パッチで署名済みのゲーム EBOOT をリネームします：

   ```text
   EBOOT.BIN -> EBOOT_ORIGINAL.BIN
   ```

7. リリース付属のブートストラップ `EBOOT.BIN` を同じ `USRDIR` フォルダに
   コピーし、元のファイル名で置き換えます。

これらの手順の後、重要なファイルは次のように配置されているはずです：

```text
/dev_hdd0/plugins/taiko/
  zucchini.sprx
  keys/

<太鼓グリーン USRDIR>/
  EBOOT.BIN
  EBOOT_ORIGINAL.BIN
```

8. USB メモリ（通常は /dev_usb000/VERSIONUP にあります）から DATA00000.BIN を
   USRDIR フォルダにコピーします。

```text
/dev_hdd0/plugins/taiko/
  zucchini.sprx
  keys/

<太鼓グリーン USRDIR>/
  EBOOT.BIN
  EBOOT_ORIGINAL.BIN
  DATA00000.BIN
```

## 初回起動

リリースの `EBOOT.BIN` はブートストラップに過ぎません。その役割は次を読み込む
ことです：

```text
/dev_hdd0/plugins/taiko/zucchini.sprx
```

初回起動時、SPRX は `EBOOT_ORIGINAL.BIN` を見つけ、パッチして再署名し、本物の
パッチ済み `EBOOT.BIN` を `USRDIR` に書き戻します。この処理中、ブートストラップ
EBOOT は脇によけられます。

初回パッチが終わったら、電源を入れ直すかゲームを再起動してください。次回起動
からはパッチ済みのゲーム EBOOT が直接使われ、通常起動にブートストラップ EBOOT
は不要になります。

`EBOOT_ORIGINAL.BIN` はそのまま残しておいてください。今後の Taiko Zucchini
アップデートで、パッチャーが変わった際にパッチ済み EBOOT を再生成するために
使うことがあります。

## プリブート MOD メニュー

MOD メニューは起動初期のプリブートの間に開けます。

- DualShock コントローラでは、ゲーム起動中に **L3 + R3** を約 3 秒間押し
  続けます。
- キーボードでは、MOD メニューが開くまで **F2** を繰り返し叩きます。キーボード
  が初期化される前に F2 を押しっぱなしにしても認識されないことがあるので、
  数回叩いてください。

## アップデート

Taiko Zucchini を更新するには、次を置き換えます：

```text
/dev_hdd0/plugins/taiko/zucchini.sprx
```

パッチ済み EBOOT はこのパスから SPRX を読み込みます。新しい SPRX が新しい
EBOOT パッチを必要とする場合、`EBOOT_ORIGINAL.BIN` から再パッチできます。

## S357 (GEX) セットアップガイド

S357 ハードウェアは現状ネットワーク越しに MOD を入れられません。MOD を読み込
ませるには、本体を「D-GEX」ハイブリッドファームウェアに変換して完全な XMB
アクセスを得てから、ゲームをインストールする必要があります。すべて PS3 の HDD
を直接マウントして行います（VM か Linux マシン）。

> ⚠️ この作業はドライブを消去／初期化します。先にバックアップを取ってください
> （手順 1）。すべてのファイルを配置した後は、上の **権限 (PERMISSIONS)** の節
> を忘れずに — `plugins/taiko` とゲームフォルダの両方に、`777` ＋ `root` を
> 再帰的に。

### 必要なもの

1. PC に RPCS3 がインストールされていること。
2. S357 の HDD をマウントするための VM または Linux 環境 — VM を使う場合は、
   できれば Linux を入れた別の PC が望ましい。
3. 最低 250 GB の空き容量。
4. S357 の HDD にアクセスできること。
5. 最低 256 MB の空きがある USB メモリ。
6. 対応する PlayStation 3 コントローラと、その Mini-USB ケーブル。汎用パッド
   （DS4／DualSense／Switch Pro コントローラ）はゲームプレイには使えますが、
   PS3 OS の扱いの都合で XMB に戻ることが **できません**。

### 事前準備

1. Linux の `dd` を使い、現在の状態の S357 HDD の `.img` バックアップを作成
   します。何か問題が起きたとき、または元に戻したいときの復元ポイントです。
2. バックアップ後、ドライブを意図的に GPT または MBR で初期化します。これに
   より S357 がセーフモードに入り、GEX のクリーンインストールができます。
3. 4.70 GEX をダウンロード：
   <https://archive.midnightchannel.net/SonyPS/Firmware/download/5addd20173bfb15b6e18461b8f928027/GEX_CRC%5B476E8B6D%5D_FW%5Bv4.70%5D_PS3UPDAT.PUP>
4. ファイル名を `PS3UPDAT.PUP` に変更し、MBR・FAT32 でフォーマットした USB
   ドライブの `PS3/UPDATE` に置きます。
5. USB を S357 に挿し、GEX の再インストール手順に従います。
6. GEX 4.70 で起動したら、初期セットアップが終わるまで待ちます（途中でエラーを
   出して再起動しますが、これは正常です）。その後 S357 の電源を抜きます。

### 「D-GEX」ハイブリッドファームウェアのセットアップ

1. 4.70 DEX をダウンロード：
   <https://archive.midnightchannel.net/SonyPS/Firmware/download/b74627dadcef86ebff0c2a424106ec4d/DEX_CRC%5B1E5390FD%5D_FW%5Bv4.70%5D_PS3UPDAT.PUP>
2. RPCS3 で `PUP` を展開します（Utilities → Extract PUP）。
3. RPCS3 で `Encrypted TAR` を展開します（Utilities → Extract Encrypted TAR）。
4. S357 の HDD を Linux でマウントします。
5. `dev_flash` を **root** で開きます。
6. `dev_flash` 内のすべてのファイルを削除します。
7. DEX の `dev_flash` から各フォルダを **大きいフォルダから順に** 一つずつ
   S357 HDD にコピーします。UFS2 が残り容量を扱う都合上、こうしないと必ず
   エラーになります。
8. アンマウントします。
9. S357 を起動すれば完全な XMB アクセスが得られます。通常の PS3 セットアップを
   案内されます。

### 太鼓グリーン（または他バージョン）のインストール

1. 太鼓グリーン 13.02 を用意します（自分でダンプするか、別途入手）。
2. `SCEEXE000` を `SCEEXE001` にリネームします。
3. `PARAM.SFO` をヘックスエディタ（HxD、ImHex など）で開き、ファイル内の
   `SCEEXE000` を `SCEEXE001` に変更します。
4. S357 の HDD を Linux でマウントします。
5. `dev_hdd` を **root** で開きます。
6. `game` に移動します。
7. `SCEEXE000` を削除します — これは GEX 再インストール初回起動時に生成される
   スタブです。
8. `SCEEXE001` を `game` にコピーします。
9. コピーが終わったら、`dev_hdd/mms/db` の `metadata_db_hdd` を削除して
   データベース復元をトリガします。これにより太鼓グリーンが独自タイトル ID で
   XMB に表示され、GEX が作ったスタブが取り除かれます。

この後は通常の **インストール** 手順で MOD を追加し、その後 **権限
(PERMISSIONS)** の修正を適用してください。

## ビルドメモ

このプロジェクトにはローカルの Sony Cell SDK インストールが必要です。SDK、
ゲームバイナリ、秘密鍵素材は含まれておらず、コミットしてはいけません。

Cell SDK ツリーを用意し、`CELL_SDK` をそこに向けます。Makefile は SDK が次の
ツールとディレクトリを提供することを期待します：

```text
<CELL_SDK>/host-linux/ppu/bin/ppu-lv2-gcc
<CELL_SDK>/host-linux/spu/bin/spu-lv2-gcc
<CELL_SDK>/host-linux/bin/ppu-lv2-prx-strip
<CELL_SDK>/host-linux/bin/make_fself
<CELL_SDK>/target/ppu/
<CELL_SDK>/target/spu/
<CELL_SDK>/target/common/
```

SDK に `host-win32/` の Windows ホスト用ツールしかない場合は、それらの `.exe`
ツールを Wine 経由で呼び出す独自の `host-linux/` ラッパー層を作り、その用意した
SDK パスを `CELL_SDK` として使ってください。

Taiko Zucchini リポジトリのルートからビルドします：

```sh
make CELL_SDK=/path/to/cell
```

デフォルトの `all` ターゲットは両方のリリース成果物をビルドします：

```text
bin/zucchini.sprx
bootstrap_eboot/bin/eboot.elf
```

ブートストラップ EBOOT だけをビルドするには：

```sh
make CELL_SDK=/path/to/cell bootstrap
```

`bootstrap_eboot/bin/eboot.elf` は未署名の初回起動用ブートストラップ EBOOT
です。自分でビルドした場合は、`EBOOT.BIN` としてインストールする前に、既存の
SELF 署名ワークフローで外部署名してください。

## リポジトリ内容

- `core/`, `hooks/`, `patches/`, `storage/`, `network/`, `input/`, `qr/`：
  プロジェクトソース。
- `bootstrap_eboot/`：初回起動用ブートストラップ EBOOT のソース。
- `eboot_patcher/`：オリジナル EBOOT の復号・パッチ・再署名・差し替えフロー。
- `vendor/mbedtls/`：Mbed TLS ソース（上流ライセンスに従う）。
- `vendor/quirc/`：quirc ソース（上流ライセンスに従う）。
- `fonts/Roboto-Medium.ttf`：Roboto フォント（Apache License 2.0）。
- `Makefile`：Cell SDK SPRX ビルド。

## 法的事項と鍵

合法的に入手したゲームファイルと鍵素材を各自で用意してください。Taiko Zucchini
は太鼓のゲーム EBOOT、Sony SDK ファイル、アプリローダ鍵、その他の専有ファイルを
配布しません。

プロジェクトコードは MIT ライセンスで公開されています。ベンダー依存物とフォント
アセットはそれぞれのライセンスに従います。`THIRD_PARTY_NOTICES.md` を参照して
ください。
