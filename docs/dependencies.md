# External Dependencies

このプロジェクトは、公開リポジトリに含めない外部コンポーネントとネイティブバイナリを前提にしています。

## 公開リポジトリに含めないもの

- `Binaries/Win64/shakoc.dll`
- `Plugins/HakoniwaPdu/Source/ThirdParty/shakoc`

## 依存物の解決順序

Win64ビルドでは、`HakoniwaPdu.Build.cs` が次の順で `shakoc` を直接参照します。

1. `HAKO_*_PATH` 環境変数
2. 箱庭Windowsインストーラの標準配置
3. リポジトリ内のフォールバック配置

使用する環境変数は次のとおりです。値にはファイルではなく、各ファイルを含むディレクトリを指定します。

| 環境変数 | 参照ファイル |
|---|---|
| `HAKO_CORE_INC_PATH` | `hako_capi.h` など |
| `HAKO_CORE_LIB_PATH` | `shakoc.lib` |
| `HAKO_CORE_DLL_PATH` | `shakoc.dll` |

標準配置は次のとおりです。

```text
Core SDK:  %APPDATA%\hakoCore-win
```

環境変数と標準配置のどちらも使用できない場合のみ、次のリポジトリ内配置へフォールバックします。

```text
Plugins/
  HakoniwaPdu/
    Source/
      ThirdParty/
        shakoc/
          lib/
            Win64/
              shakoc.lib
Binaries/
  Win64/
    shakoc.dll
```

実行時のDLLローダーも、環境変数、標準配置、ビルド時に解決したディレクトリ、リポジトリ内配置の順で `shakoc.dll` を探索します。

## Windowsセットアップスクリプト

Windowsでは、リポジトリルートから次のスクリプトを実行すると、環境変数ベースの依存解決を検証し、Editorターゲットをビルドできます。依存物をリポジトリ内へコピーしません。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\windows\setup_build.ps1
```

スクリプトは `HAKO_*_PATH` 環境変数、標準インストール先、リポジトリ内フォールバックの順で `shakoc` を検証します。`-CoreSdkRoot` を指定した場合は、そのルートからプロセスローカルの `HAKO_CORE_*_PATH` を設定してビルドします。

```text
Core SDK:  %APPDATA%\hakoCore-win
UE:        %ProgramFiles%\Epic Games\UE_<EngineAssociation>
```

別の場所にSDKまたはUnreal Engineを配置している場合は、ルートを明示できます。

```powershell
.\scripts\windows\setup_build.ps1 `
  -EngineRoot 'D:\Epic Games\UE_5.8' `
  -CoreSdkRoot 'D:\sdk\hakoCore-win'
```

診断だけを行い、ビルドを実行しない場合:

```powershell
.\scripts\windows\setup_build.ps1 -ValidateOnly
```

`hakoniwa-pdu-registry` が未取得の場合は、先に次を実行してください。

```powershell
git submodule update --init --recursive
```

## 現時点の制約

Win64 だけを対象にしています。★ 標準配置の `%APPDATA%\hakoCore-win` は古い SDK のことがあります。ローバ用のローカル SDK に合わせる作業はこれからです（環境変数 `HAKO_CORE_*_PATH` を設定すれば、ビルド時も実行時もそちらが優先されます）。

依存物の取得元、ビルド方法、バージョン固定方法は今後のビルド手順で整理します。
