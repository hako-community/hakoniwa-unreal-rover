# Hakoniwa Unreal Rover

月面ローバの箱庭シミュレータ用の Unreal Engine 5.8 プロジェクトです。南極域の実地形（DEM）・位置暦の太陽と地球・物理カメラの露出で、月面の場面を描きます。

* [hakoniwa-unreal-drone](https://github.com/hako-community/hakoniwa-unreal-drone) をコピーして始め、ドローンの部分（機体の C++・`HakoniwaDroneService`・ドローンの Level と Blueprint・hakodrone の取得）は外しました（2026-10-18）。
  箱庭の PDU 通信（`Plugins/HakoniwaPdu`）は残しています（ローバの状態の同期に使う予定）。
* 物理は [hakoniwa-mujoco-rover](https://github.com/hako-community/hakoniwa-mujoco-rover) が受け持ちます。このプロジェクトは描画だけです。

## セットアップ

1. Unreal Engine 5.8 をインストールします。
2. hakoniwa-mujoco-rover を**隣のフォルダ**に置きます（地形・位置暦・ローバの GLB を読むため）。
3. 箱庭コアの `shakoc`（`HakoniwaPdu` が使う）を用意し、ビルドします。

```powershell
pwsh scripts\windows\setup_build.ps1
```

`shakoc` の場所は `HAKO_CORE_INC_PATH` / `HAKO_CORE_LIB_PATH` / `HAKO_CORE_DLL_PATH`（いずれもフォルダ）→ `%APPDATA%\hakoCore-win` → リポジトリ内の順で探します（[docs/dependencies.md](docs/dependencies.md)）。
ローバ用のローカル SDK に合わせる作業はこれからです。

## 月面の場面

場面は生成物で、元データは [hakoniwa-mujoco-rover](https://github.com/hako-community/hakoniwa-mujoco-rover) の地形タイル（`grid.json`・16bit 高さ画像）・位置暦（`sun.json`）・ローバの GLB です（隣のフォルダに置く）。

```powershell
pwsh scripts\windows\setup_build.ps1                 # ビルド（Windows PowerShell 5.1 でも可。スクリプトは BOM 付き UTF-8）
pwsh scripts\windows\rover_scene.ps1 -Build          # /Game/Rover/Levels/LunarSouthPole を作る（★ clone の後に必須。約 30 秒。エディタが自動で開いて閉じる）
#   地図・岩のメッシュ・マテリアル（Content/Rover/Levels・Terrain・Materials）は生成物で git 管理外（0.5 m の地形で地図が 100 MB を超えるため）
pwsh scripts\windows\rover_scene.ps1 -Shots -Probe   # -game で開き、地形の高さを検査して 4 構図を撮る（Saved\RoverShots\<時刻>\）
pwsh scripts\windows\rover_scene.ps1 -Open           # エディタで開く（PIE は最初のカメラから見る）
pwsh scripts\windows\rover_scene.ps1 -Compare        # 参照カメラ（mujoco-rover の ref_cameras.json）の構図で撮り、参照の絵と並べる
```

| 要素 | 中身 |
|---|---|
| 地形 | 16bit 高さ画像 → Landscape（442×442 頂点。DEM の外は端の値を延ばす）。**ヨー 90° で置き**、四角形を割る対角線の向きを MuJoCo の HField とそろえる。同じ 500 点で Unreal − MuJoCo は最大 0.003 m |
| 座標 | 局所（右手系 m・x 前 y 左 z 上）→ Unreal = (100x, −100y, 100z)（`RoverFrame.h`） |
| 光 | 太陽（平行光・`sun.json` の向き・照度・視直径 0.53°）、地球（50 km 先の球。太陽に照らされて満ち欠けが出る）、地球照（弱い平行光）。空の光・大気・霧は置かない |
| レゴリス | Hapke の反射（単一散乱アルベド 0.32・衝効果 B0 1.0・h 0.06・2 項 HG b 0.21・c 0.70・多重散乱の H 関数）をマテリアルで計算（太陽の向きは `MPC_Lunar`）。5 m 格子より細かい凹凸（6〜490 cm の 5 段の勾配ノイズ）を法線に、明るさのむら（±15%）を色に |
| 写り | 自動露出なし・物理カメラ（ISO 100・1/125 s・f/16）。局所露出は切る（`RoverLunarLook.h`） |
| ローバ | GLB を 1 メッシュに結合して取り込み、原点に地形の傾きに合わせて置く（静止。関節の同期はこれから）。★ メッシュは git に入れない（`/Content/Rover/Vehicles` は管理外）。clone したら `-Build` で各自の PC で作る（GLB は `-Glb` で指定、省略時は mujoco-rover の `bodies\*\source\*.glb`）。作るまでは地図にローバが映らない |
| 撮影 | `ARoverShotDirector`：タグ `RoverShot` のシネカメラを順に撮り、フレーム時間を `shots.json` に書く。`-RoverProbeIn/Out` で地形の高さを測る |

* ★ `ADirectionalLight` は生成時に光へピッチ −46° を入れている。スポーンの回転に足されるので、回転は生成のあとで決める。
* ★ `AStaticMeshActor` の根はメッシュのコンポーネントそのもの。メッシュの向きの補正を相対回転で入れるとアクタの向きを上書きするので、体の向き × 補正を 1 つの回転にして置く。
* 手で組むときはエディタのコンソールで `Rover.BuildScene -RoverGrid=<grid.json> -RoverSun=<sun.json> -RoverGlb=<glb>`。

## ライセンス

本リポジトリは MIT License の下で公開されています。詳細は `LICENSE` ファイルを参照してください。
