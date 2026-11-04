# VoodooELAN — ELAN 触摸屏 → macOS 原生手势

把联想天骄16 一体机（作为外接显示器）上的 **ELAN USB 触摸屏（04F3:2F05，10 点）**，
在 Hackintosh 里用**原生 kext 驱动**接入 macOS 触控板引擎，获得原生手势
（滚动 / 缩放 / 双指右键 / 三指四指切屏 / 点按打开应用），**不用 TJ16-touchscreen 这类 app**。

## 原理

```
VoodooELAN.kext（本工程，需编译）
  ├─ 匹配 USB 04F3:2F05（IOUSBHostInterface，高 IOProbeScore 抢过 Apple 通用 HID 驱动）
  ├─ 内核里直接读 HID 中断报告
  ├─ 解析 ELAN 多点（report[0]=0x01，末字节=点数，X 0–3168 / Y 0–1968）
  └─ 组装 VoodooInputEvent 喂给
VoodooInput.kext（官方预编译，需一并装）
  └─ 伪装成 Magic Trackpad 2 → 苹果原生 Multitouch 引擎 → 原生手势
```

客户端通过设 `VoodooInputSupported=true` 属性让官方 VoodooInput 自动挂载到本驱动上，
事件走 `kIOMessageVoodooInputMessage`（12345）发送。

## 编译（无 macOS 环境也能编）

推到 GitHub 仓库后，Actions 里的 `Build VoodooELAN` 工作流会用 macos-15 runner 编译，
产物下载为 `VoodooELAN-kext`（内含 `VoodooELAN.kext`）。

本地 macOS 上也可直接 `./build.sh`（依赖 MacKernelSDK，会自动下载）。

## 安装到 OpenCore

1. 下载两个 kext：
   - 本工程编译出的 `VoodooELAN.kext`
   - 官方 VoodooInput：`https://github.com/acidanthera/VoodooInput/releases`
     （`VoodooInput-V1.1.7-RELEASE.zip`，取里面的 `VoodooInput.kext`）
2. 把 **两个都** 拷进 `EFI/OC/Kexts/`。
3. 用 OCC / ProperTree 在 `config.plist → Kernel → Add` 里加这两项，
   顺序：`VoodooELAN.kext` 在 `VoodooInput.kext` **之后**（先加载 VoodooInput 提供类，
   再加载依赖它的客户端更稳；两者都启用）。
   - `VoodooELAN.kext` BundleID：`com.loverlf.VoodooELAN`
   - `VoodooInput.kext` BundleID：`me.kishorprins.VoodooInput`
4. `Kernel → Quirks` 的 `DisableLinkeditJettison`、`AppleXcpmCfgLock` 等按你现有黑苹果配置。
   重启。

## 验证

开机后：

```bash
# 1) 驱动有没有认领设备（应出现 VoodooELAN 挂在 04F3:2F05 上）
ioreg -r -l -c VoodooELAN

# 2) VoodooInput 是否挂上（应有 "VoodooInput Instance"=true，且有 VoodooInputSimulatorDevice）
ioreg -r -l -c VoodooInput

# 3) 系统是否生成触控板（出现 Trackpad 设备）
ioreg -r -l | grep -i "AppleMultitouchDevice\|MT Simulator" 

# 4) 看内核日志有没有报错
log show --last 5m --predicate 'eventMessage contains "VoodooELAN"'
```

然后：**系统设置 → 触控板** 应出现触控板选项；触摸屏上试滚动 / 双指 / 点按。

## 已知限制 / 待验证项

- **手指追踪**：当前每个触点用「槽位号」当 `secondaryId`，点按/滚动在手指顺序稳定时正常；
  若要三指四指切换更稳，后续可加按距离匹配的跨帧追踪（VoodooI2C 的做法）。
- **物理尺寸**：`Physical Max X/Y` 先填了原始坐标最大值（1:1），若手势速度/比例不对再调。
- **坐标方向**：默认未翻转。若触摸方向反了，改 `VoodooELAN.cpp` 里 `start()` 的
  `IOFBTransform`（0=正，`kIOFBRotate90/180/270` 见 VoodooInput 的 MultitouchHelpers）。
- **抢设备**：靠 `IOProbeScore=1000` + 精确 VID/PID 抢过 Apple 通用 HID 驱动；
  若装后「HID 兼容触摸屏」仍被苹果接管、驱动没挂上，需要再加个性 or 调分。

## 报告格式参考

解析逻辑照搬自 `niu541412/TJ16-touchscreen` 的 `parse_touch_report()`，
坐标范围与槽位偏移完全一致。
