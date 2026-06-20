# 钙反应 pH 控制器

ESP32 本地网页控制器。当前版本只检测一路钙反应 pH，并控制一路继电器。

## 低压接线

### ADS1115

ADS1115 使用 `3.3V` 供电，地址设为 `0x48`。

| ADS1115 | ESP32 |
| --- | --- |
| VDD | 3V3 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| ADDR | GND |
| A0 | 直接接 pH 模块 PO/AO 模拟输出 |

### PH-4502C

PH-4502C 使用 `5V` 供电。模块上的 `PO` 是模拟输出，也有商家标为
`AO`。当前实物接线没有额外分压电阻，`PO/AO` 直接接入 ADS1115 的
`A0`。

| PH-4502C | 连接 |
| --- | --- |
| VCC | ESP32 开发板 5V |
| GND | ESP32 GND |
| PO/AO | 直接接 ADS1115 A0 |

### 5V 继电器模块

当前现场使用的继电器模块为高电平触发。如果模块带有触发方式跳帽，请设置为
`HIGH`。

| 继电器低压端 | ESP32 |
| --- | --- |
| DC+ | 5V |
| DC- | GND |
| IN | GPIO26 |

## 220V 安全

ESP32、ADS1115 和 PH-4502C 只接低压侧。220V 电磁阀只能接继电器的
`COM` 与 `NO` 输出端。接线、测试和改线时必须断开市电。建议由熟悉市电
布线的人完成，并加保险丝、绝缘外壳和应力保护。

## 网页

ESP32 会同时开启自己的热点，并尝试连接你配置的家庭 Wi-Fi。

手机直接连接 ESP32 热点：

```text
热点名称：AquaPH-Controller
热点密码：aquaph1234
网页地址：http://192.168.4.1
```

如果 ESP32 也连上了家庭 Wi-Fi，同一网络中的电脑或手机也可以访问：

```text
http://aquaph.local
```

如果路由器不支持本地域名，请从串口日志中读取 ESP32 的 IP 地址并直接
访问。

网页支持：

- 查看原始电压、滤波电压和 pH
- 手动开关继电器
- 设置自动控制的开启与关闭 pH
- 记录 pH 7.00 与 pH 4.00 标准液，完成两点校准
- 保存所有设置，断电后自动恢复
- 通过 Wi-Fi OTA 远程烧录新固件

自动控制默认关闭。完成低压测试和 pH 校准后，再在网页中主动启用。

## 树莓派 USB 串口接入

当前固件保留本地网页，同时增加 USB 串口 JSON Lines 协议，供
MyReef 树莓派页面读取状态、保存设置、手动开关阀门和触发校准。ESP32
仍然独立执行 pH 采样、滤波、自动控制和配置保存；树莓派断开后，ESP32
继续按最后保存的设置运行。

每条命令是一行 JSON，以 `\n` 结尾；ESP32 返回一行 JSON，成功响应包含
`ok: true` 和 `snapshot`。

示例：

```json
{"cmd":"status","id":"pi-1"}
{"cmd":"settings","id":"pi-2","auto_enabled":true,"open_ph":6.70,"close_ph":6.50}
{"cmd":"relay","id":"pi-3","on":false}
{"cmd":"calibrate","id":"pi-4","point":7}
{"cmd":"calibrate_reset","id":"pi-5"}
```

支持命令：

- `hello`
- `status`
- `heartbeat`
- `settings`
- `mode`
- `relay`
- `calibrate`
- `calibrate_reset`

## 编译与烧录

```bash
pio run
pio run --target upload
pio device monitor
```

USB 烧录过带 OTA 的固件后，可以通过局域网远程烧录：

```bash
pio run -e esp32dev_ota --target upload
```

默认 OTA 地址是 `10.0.0.7`，默认 OTA 密码是 `aquaph1234`。如果 ESP32 的
IP 改变，可以临时指定：

```bash
pio run -e esp32dev_ota --target upload --upload-port 你的ESP32_IP
```

Wi-Fi 密码保存在本机忽略文件 `include/secrets.h` 中，不会加入 Git。
热点名称和密码也可以在 `include/secrets.h` 中通过 `WIFI_AP_SSID` 与
`WIFI_AP_PASSWORD` 修改。热点密码至少需要 8 位。
OTA 密码可以通过 `WIFI_OTA_PASSWORD` 修改；如果修改了，也要同步更新
`platformio.ini` 里的 `esp32dev_ota.upload_flags`。
