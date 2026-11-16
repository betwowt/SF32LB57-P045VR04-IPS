# SDK 补丁说明

本工程的 `../SiFli-SDK` 必须建立在下列基线上,再按序号打补丁。

## 基线

| 项 | 值 |
|---|---|
| 仓库 | `https://gitee.com/SiFli/sifli-sdk.git` |
| 分支 | `release/v2.5` |
| **基线提交** | `504eb2225a2f1ca86417dfd145a94e3fc21071c7` |
| 基线说明 | `[chore][example] Add missing doc index file for spi example` |
| 上一处端口提交(仅本地) | `7bdbae345eeb1a76494d660620af2749a21912d8` `Port tweaks for the SF32LB57 xiaozhi voice device` |

`7bdbae34` 是本地提交,内容等价于本目录 `0001`–`0004`;远端 `origin/release/v2.5` 仍停在 `504eb222`。

## 补丁

| 序号 | 文件 | 内容 |
|---:|---|---|
| 0001 | `0001-add-p045vr04-panel.patch` | `Kconfig_lcd` 增加 P045VR04 面板项 |
| 0002 | `0002-lcdc-dpi-linebuf-height.patch` | LCDC DPI 行缓冲高度 |
| 0003 | `0003-link-lds-ezip-font-sections.patch` | 链接脚本增加字体段 |
| 0004 | `0004-sdk-wifi-lwip-tls-tweaks.patch` | AIC8800 SDIO/lwIP/TLS/WebSocket 端口调整(8 个文件) |
| 0005 | `0005-pdm1-stereo-barge-in.patch` | PDM1 立体声采集 + `audio_server` 双声道下行混音与 `AUDIO_IOCTL_DROP_TX_CACHE`(2 个文件) |

## 应用顺序

```bash
cd ../SiFli-SDK
git checkout 504eb2225a2f1ca86417dfd145a94e3fc21071c7
for p in ../SF32LB57-P045VR04-IPS/patches/000{1,2,3,4,5}-*.patch; do
    git apply "$p" || exit 1
done
```

## 构建产物(不要提交)

`external/mbedtls_228/SConscript` 在构建时**生成** `ports/src/tls_certificate.c`;AIC8800 的
`lib/libaic8800_driver.a` 与 `platform/**/*.o` 也会被构建改写。这些都属于构建产物,
构建后如需保持工作区干净:

```bash
cd ../SiFli-SDK
git checkout -- external/mbedtls_228/ports/src/tls_certificate.c \
    customer/peripherals/wifi/aic8800d40-n/lib/libaic8800_driver.a
git status --porcelain | awk '{print $2}' | grep -E '\.(o|a)$' | xargs -r git checkout --
```

## 校验

在当前工作区对每个补丁做反向检查(全部 OK 表示工作区 == 基线 + 补丁):

```bash
cd C:/Users/betwowt/Documents/project/SF32LB57-P045VR04-IPS
for p in 0005 0004 0003 0002 0001; do
    git -C ../SiFli-SDK apply -R --check "$(pwd)/patches/${p}-"*.patch && echo "$p OK"
done
```
