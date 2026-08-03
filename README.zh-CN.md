<p align="right"><a href="README.md">English</a> | <b>简体中文</b></p>

# sesame agent esp32

一个住在 ESP32-S3 上的 AI agent，直接控制它自己所运行的硬件。

用日常语言让它拍张照、扫一遍 I2C 总线、转动舵机、录一段音频、或者抓一个网页，它都在板子上自己完成。没有配套 App，也没有中间的云服务。它唯一往外发的请求，就是你指定的那个模型 API。

<p align="center"><img src="docs/board.svg" alt="板子、引脚分配和三颗 LED" width="560"></p>

## 它是怎么工作的

设备上跑着一个小型 shell。同一套命令有四条路可以调用：USB 串口、SSH、HTTP，以及模型把它当作唯一的工具来调用。加一条命令，四个入口同时就有了。模型的 system prompt 是从当前的命令表实时生成的，所以它永远清楚这一版固件到底能做什么。

模型用哪家由你决定。只要是 OpenAI chat completions 格式就行，DeepSeek、OpenAI、OpenRouter、Groq、Together，或者本地跑的 llama.cpp 都可以。API key 存在设备上，除了访问你指定的接口之外不会发去别处。

![网页界面](docs/chat.png)

## 烧录

仓库里已经带了编译好的固件 [`firmware/sesame-agent-esp32.bin`](firmware/sesame-agent-esp32.bin)，clone 下来就什么都有了。它是一个合并好的单文件，bootloader、分区表和应用都在各自正确的偏移上，烧录不需要装工具链。

```
pip install esptool
git clone https://github.com/vcup-date/sesame-agent-esp32.git
cd sesame-agent-esp32
```

按住 BOOT，点一下 RESET，松开 BOOT，然后：

```
esptool.py --chip esp32s3 --baud 460800 write_flash 0x0 firmware/sesame-agent-esp32.bin
```

这就装完了。不想 clone 的话，同一个文件也挂在 [latest release](../../releases/latest) 上。

这个镜像从偏移 0 开始，会覆盖 NVS 分区，所以烧完之后已保存的 Wi-Fi、API key 和各项设置都会被清掉，设备重新进入配网模式。新板子正是要这样。如果只是给一块已经配好的板子重新刷固件、又想保留设置，就只写应用分区：

```
esptool.py --chip esp32s3 --baud 460800 write_flash 0x10000 build/sesame-agent-esp32.bin
```

如果识别不到板子，先确认数据线是能传数据的，而不是只能充电的那种。这是最常见的原因。macOS 上端口一般是 `/dev/cu.usbserial-*`，Linux 上是 `/dev/ttyUSB0`，Windows 上是某个 COM 口。esptool 猜错的话用 `-p` 指定。

## 让它连上你的网络

没有任何东西是写死的。第一次开机时设备没有凭据，于是它自己开一个 Wi-Fi 等你来配。

1. 上电。它会开一个叫 `sesame-agent` 的**开放**热点。没有密码，因为「为了打开一个填密码的页面，先要填一次密码」实在是很糟糕的开场。
2. 用手机或电脑连上这个热点。大多数设备会自动弹出配置页，用的是 DHCP option 114，DNS 劫持作为兜底。没弹出来就手动打开 `http://192.168.4.1`。
3. 选你的网络、填密码，顺手可以粘贴 API key。key 也可以先跳过、以后再填。
4. 设备连上你的网络，配网热点随即消失。

![配网](docs/setup.png)

列表里只会出现 **2.4 GHz** 的网络。ESP32-S3 根本没有 5 GHz 射频，所以一直找不到的那个网络，基本可以确定是 5 GHz 独占的。现在的 Mesh 路由器常把双频合并显示，很多人就是卡在这里。

连上之后，用 `http://sesame-xxxx.local` 访问，`xxxx` 来自板子的 MAC。每块板名字都不一样，所以同一个网络里放好几块也不会撞名。具体名字会在开机时打到串口，配网页面上也会显示。

之后想换网络，可以用网页界面里的设置面板，或者在任意控制台执行 `wifi connect <ssid> <密码>`。想完全重来就 `wifi forget` 再重启。

## SSH

SSH 服务默认开启，账号密码是 **admin / admin**，板子一连上网就能用。

```
ssh admin@sesame-xxxx.local
passwd 换一个像样的密码
```

请务必改掉。任何能访问到 22 端口的人都等于拿到了这块板子的完整控制权；默认密码还在的时候，登录时会提醒你。

`agent` 命令会把会话切进对话模式，之后每一行都直接发给模型，工具调用会实时显示出来。`exit` 回到 shell。

```
sesame-ede4> agent
conversation mode. ask in plain language; 'exit' returns to the shell.
sesame-ede4 ask> i2c 总线上挂了什么？
  · i2c scan
    0x3c
0x3c 上有一个设备，这个地址通常是 SSD1306 OLED。
sesame-ede4 ask> exit
```

## 网页界面

由板子自己提供，大约 60 KB，没有外部资源、没有框架、没有任何追踪。断网也能在局域网里用。

- 对话中直接内嵌工具调用，跑了什么命令、返回了什么，一眼可见
- 会话历史，刷新后回到上一次的对话而不是空白页
- 停止按钮在生成过程中真的能用：它由 81 端口上的第二个 HTTP 服务提供，所以主服务在忙也不影响
- 文件浏览，可下载、可删除，并显示剩余空间
- Wi-Fi 扫描和连接
- 模型配置档，可以同时留着 DeepSeek 和本地模型随时切换
- 明暗两套主题，中英文界面
- 相机拍的照片会显示成缩略图，可以点开和下载

它是照着手机和电脑同等重要来做的。

<p align="center"><img src="docs/mobile.png" width="330" alt="手机界面"></p>

![状态面板](docs/status.png)

## 可以让它做什么

这些都是真的能跑通的例子：

- 「拍张照片，然后告诉我还剩多少空间」
- 「I2C 总线上挂了什么？」
- 「抓一下 example.com，存下来，再用一句话总结」
- 「让 LED 闪三下，然后报一下芯片温度」
- 「扫一下蓝牙设备，列出最近的三个」
- 「写一个数到十的 Python 脚本，存下来并运行」

碰到单条命令表达不了的逻辑，它会写一段 MicroPython 直接在设备上跑。

## 命令

四个入口都能用：串口控制台、SSH、`POST /api/exec`，以及 agent 的 `shell` 工具。

### 文件与文本

| | |
|---|---|
| `ls [dir]` | 长格式列目录：类型、大小、名字 |
| `cat <file>` | 打印文件 |
| `write <file> <text>` | 写入文本，换行和缩进原样保留 |
| `cp <src> <dst>` | 复制，逐字节一致，二进制安全 |
| `mv <src> <dst>` | 重命名或移动 |
| `rm <file>` · `rmdir <dir>` · `mkdir <dir>` · `touch <file>` | 字面意思 |
| `head <file> [n]` · `tail <file> [n]` | 头部或尾部若干行，最多 2000 |
| `grep <text> <file>` | 带行号的匹配行，最多 400 条 |
| `wc <file>` | 行数、词数、字节数 |
| `tree [dir]` | 递归列出并带大小，最深 12 层 |
| `df` · `free` | Flash 剩余空间，以及 SRAM 和 PSRAM 堆 |
| `text <file.html> [chars]` | 把 HTML 剥成可读文本 |

### 网络

| | |
|---|---|
| `wifi [status\|scan\|connect <ssid> <pw>\|static <ip> <gw>\|dhcp\|forget]` | 连接、查看，或固定地址 |
| `ping <host> [count]` | ICMP，逐包 RTT 和丢包率 |
| `resolve <host>` · `netstat` | DNS 查询；网卡、DNS 和监听端口 |
| `http <url> [-o <file>]` | HTTPS 带证书校验，可直接流式写入文件 |
| `time [sync]` | 时钟，NTP |
| `sniff [seconds] [channel]` | 被动统计各发射源的帧数，只读帧头 |
| `peer [list\|send <name\|all> <text>\|inbox]` | 通过 ESP-NOW 和其它板子通信，4 跳 mesh |

### Agent、shell 与定时任务

| | |
|---|---|
| `agent` | 进入对话模式，`exit` 退出 |
| `agent status\|reset` | 模型、上下文、缓存命中率；清空对话 |
| `ask <request>` | 只问一次，不进入交互 |
| `seq [xN] <cmd>; <cmd>; ...` | 一次调用里跑完整串命令，用 `wait <ms>` 控制间隔 |
| `cron add [-g] [-k] <secs\|once> <cmd>` | 定时任务；`-g` 重启后仍在，`-k` 永不过期 |
| `cron list\|del <id\|all>\|pause\|resume` | 管理任务 |
| `skill list\|show <name>\|rm <name>` | 存下来的操作步骤，用到时再读 |
| `python` | 交互式会话，`>>>` 和 `...`，`exit` 退出 |
| `python <code>` · `python -f <file>` | 不进入交互直接执行（`py` 是简写） |
| `cfg [list\|get\|set]` · `passwd <pw> [user]` | 设置项；修改 SSH 密码 |
| `ssh` · `sysinfo` · `uptime` · `reboot` · `help [cmd]` | |

`seq` 比看上去重要。每次工具调用都是一次网络往返，所以一条一条发出去的颜色渐变不但慢，还可能把单轮的调用次数用光。`seq x3 rgb 255 0 0; wait 150; rgb 0 0 255; wait 150` 只是一次调用，节奏由设备自己掌握。

### Shell 操作符

```
sysinfo > /sesame/info.txt        输出写入文件
uptime >> /sesame/info.txt        追加而不是覆盖
ls | grep py                      管道接给过滤命令
ls | grep sesame | wc             可以串起来
snap && ls /sesame/photos         前一条成功才跑后一条
```

这台设备上没有 stdin：命令拿到的是 argv，不是数据流。所以管道的做法是把左边的输出抓到一个临时文件，再把这个路径作为多出来的一个参数交给右边。这恰好符合过滤类命令的用法，因为 `grep`、`head`、`tail`、`wc`、`text` 的最后一个参数都是路径，于是 `ls | grep py` 实际执行的是 `grep py <临时文件>`。如果右边那条命令根本不接受文件参数，管道对它就没有意义。

`write` 和 `python` 不参与解析，因为它们会把整行原样收下。文件正文里的 `>`、Python 代码里的 `|`，都保持字面意思。

一条管道最多 6 段，`>` 只能写在最末尾。写在中间会直接报错，而不是被悄悄忽略；目标文件写不进去时也会明确报错，而不是假装成功。

### 硬件

引脚号就是 GPIO 号。`pins` 会列出哪些还空着。

**`gpio get <pin> [up|down]` · `gpio set <pin> <0|1>`**
读一个引脚（可选内部上下拉），或者驱动它。上下拉的意义在于区分「悬空」和「被什么东西拉住」：开了上拉之后，没接东西的脚读 1，而被拉低的脚仍然读 0。
```
gpio get 4 up        →  1  (pull-up)
gpio set 2 1
```

**`adc <channel 0-9>`** ADC1，大约 0 到 3100 mV，12 位。
```
adc 3                →  channel 3: raw 2048 (~1550 mV)
```

**`pwm <pin> <duty 0-255> [freq]`** LEDC，默认 5 kHz。适合调亮度和转速，不适合舵机：在 50 Hz 下 8 位占空比只能分出十来个可用档位。
```
pwm 2 128 1000
```

**`servo <pin> <angle 0-180> [min_us] [max_us]` · `servo <pin> us <n>` · `servo release <pin>`**
MCPWM，1 MHz 计数，所以脉宽直接以微秒设定，也就是舵机手册上用的单位。默认 500 到 2500 us。`release` 停掉脉冲，舵机随之松力。舵机要单独供电，不要接 3V3 引脚。
```
servo 2 90           →  servo 2: 1500 us (90 deg)
servo 2 us 1750
```

**`stepper step <step_pin> <dir_pin> <steps> [rpm]`**
**`stepper 4wire <p1> <p2> <p3> <p4> <steps> [rpm]`**
`step` 驱动 A4988、DRV8825 这一类；`4wire` 驱动经 ULN2003 的 28BYJ-48。步数为负则反转。结束时会断掉线圈电流，免得电机一直发烫。执行期间会阻塞，上限 25 秒。
```
stepper 4wire 1 2 14 21 2048 15      →  正转 2048 步，15 rpm
stepper step 4 5 -400 120
```

**`dac <pin> <level 0-255>` · `dac tone <pin> <hz> [ms]` · `dac play <pin> <hz:ms,...>` · `dac off`**
S3 没有 DAC。这里用的是 sigma-delta 调制器，输出的脉冲密度经过一个 RC 低通（1k 加 100nF 就行）之后，就是真正的模拟电压或正弦波。不加滤波的话，那个引脚仍然只是在快速翻转的数字口。
```
dac 2 128                              →  VDD 的一半
dac tone 2 440 500                     →  真正的 440 Hz 正弦
dac play 2 523:150,659:150,784:300     →  C E G
```

**`tone <pin> <hz> [ms]`** 给无源蜂鸣器的方波。比 `dac` 粗糙，但不需要滤波。

**`ir rx <pin> [ms]` · `ir tx <pin> <+mark,-space,...>`**
以微秒记录原始的高低电平时长，再通过 38 kHz 载波发回去。不解析任何协议，所以对着没有文档的遥控器也能用。`rx` 的输出可以直接粘给 `tx`。录制需要 TSOP38238 之类的解调接收头，发送需要一颗红外 LED。
```
ir rx 14 8000        →  +9000,-4500,+560,-560,...
ir tx 2 +9000,-4500,+560,-560
```

**`i2c scan [sda] [scl]`** i2cdetect 那种网格。默认扫相机那条总线（sda 4，scl 5），是共用而不是抢占。

**`i2creg read <addr> <reg> [n]` · `i2creg write <addr> <reg> <hex...>`**
不写驱动直接读写寄存器。地址和寄存器都用十六进制。
```
i2creg read 3c 00 2
i2creg write 3c 00 af
```

**`spi <sclk> <mosi> <miso|-1> <cs> <hex...>`** 在 SPI2 上做一次 1 MHz 全双工传输。SPI1 是 Flash 和 PSRAM，碰不得。

**`i2s tone|play <bclk> <lrclk> <dout> ...`** 输出到 MAX98357A 这类功放。
**`i2s rec <bclk> <ws> <din> <file.wav> [secs] [rate]`**
**`i2s pdmrec <clk> <din> <file.wav> [secs] [rate]`**
从 INMP441、SPH0645 或 PDM 麦克风录制 16 位单声道 WAV。会报告峰值电平，所以一段静音文件读起来是接线问题，而不是「成功了」。

**`rgb <r> <g> <b> [pin]` · `rgb off`** GPIO48 上的 WS2812，走 RMT 驱动。普通的 `gpio set` 点不亮它：它要的是有严格时序的位编码。

**`led [status|idle|think|ok|error|off]`** 同一颗灯上的状态动画。空闲时蓝色呼吸，思考时琥珀色脉动，成功闪绿，出错闪红。

**`ble [status|scan [secs]|adv on|off]`** 扫描附近设备，或者把自己广播出去。只做广播端，所以没有东西能连进来。协议栈大约占 58 KB 内部 RAM，扫描结束后会释放掉。

**`snap` · `camera [status|pins]`** 拍照存到 `/sesame/photos`，或者查看传感器和它的引脚分配。

**`disp <text>`** 挂在相机 I2C 总线上的 SSD1306 OLED。

**`uart <tx> <rx> <baud> [text]`** 用 UART1 和 GPS、模组或另一块板子通信。UART0 是控制台，刻意不开放。

**`mon <heap|adc <ch>|gpio <pin>> [samples] [ms]`** 按时间采样并画出折线，回答的是「它在漂吗」，而不是「它现在是多少」。

**`pins [free]`** 每个 GPIO 的实时占用情况。

**`temp` · `rand [n]` · `sleep <secs>`** 芯片温度、硬件随机数、按定时或 GPIO 唤醒的深度睡眠。

## 这块板子的引脚

相机占掉了大部分低编号 GPIO。`pins` 会实时打印，这里列一份备查：

| 用途 | GPIO |
|---|---|
| 相机 XCLK、SIOD、SIOC | 15, 4, 5 |
| 相机数据 D0 到 D7 | 11, 9, 8, 10, 12, 18, 17, 16 |
| 相机 VSYNC、HREF、PCLK | 6, 7, 13 |
| WS2812 状态灯 | 48 |
| microSD CMD、CLK、DATA | 38, 39, 40 |
| UART 控制台 | 43, 44 |
| 空闲可用 | 1, 2, 14, 21, 41, 42, 47 |

有两段是危险的，软件里直接禁掉了。GPIO 26 到 32 是 SPI Flash。GPIO 33 到 37 是这个模组上的八线 PSRAM 总线，去驱动其中任何一根，芯片会当场死掉，连报错的机会都没有。

板上有三颗 LED。红色那颗直接跨在 3.3 V 上，没有 GPIO，所以它一直亮着，软件改不了。蓝色那颗接在 GPIO43，而这根线同时是 UART 发送脚，所以它会随着控制台输出闪烁；想拿它当状态灯，就得放弃串口控制台。这版固件用的是 GPIO48 上那颗可寻址 RGB：空闲时蓝色呼吸，思考时琥珀色脉动，成功闪绿，出错闪红。

## 从源码编译

需要 ESP-IDF v5.5。先加载它的环境变量，否则 `idf.py` 不在 PATH 上，下面每条命令都会报 "command not found"：

```
. $HOME/esp/esp-idf/export.sh        # Windows: %USERPROFILE%\esp\esp-idf\export.bat
```

然后：

```
git clone https://github.com/vcup-date/sesame-agent-esp32.git
cd sesame-agent-esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py` 本身就是可执行文件，直接运行即可，不需要写成 `python idf.py`。

`components/micropython/micropython_embed` 下的 MicroPython 嵌入包是生成产物，之所以一起提交，是为了让新 clone 下来的仓库不用额外步骤就能编译。改过 `mpconfigport.h` 之后要重新生成，在本地有 MicroPython 源码的前提下运行 `components/micropython/regen.sh`。

`tools/smoke.py` 在烧录后通过串口和 HTTP 做一遍检查，覆盖启动完整性、文件系统、Python、GPIO 和网页接口。

## 已知边界

这个 shell 不是 bash。有 `|`、`>`、`>>` 和 `&&`，但没有通配符、没有变量、没有命令替换、也没有子 shell。管道是把临时文件交给下一条命令，而不是真正的流，所以只对接受文件路径的命令有意义。`seq` 覆盖了「几步带节奏地连起来跑」这个需求，但它是一张清单，不是一门语言。

这里的 MicroPython 只做计算。`python` 会开一个真正的交互式会话，有 `>>>` 和 `...` 提示符、表达式回显和跨行保持的状态，但里面没有 `machine` 模块，也访问不了文件和引脚。凡是碰硬件的都必须是 C 命令，这也正是命令列表这么长的原因。

`peer` 的消息既没有加密也没有签名。射频范围内的任何人都能读，包里的名字是一个声称，不是凭证。

网页界面没有任何鉴权，SSH 也带着默认密码。两者都等于板子的完整控制权，包括执行任意命令。这是按「你信任的网络」来设计的。请用 `passwd` 改掉 SSH 密码，并且在没有加上真正的鉴权之前，不要把它们暴露到公网。

相机、Wi-Fi、SSH、网页界面、ESP-NOW、BLE、sigma-delta 输出和红外发送都已在真实硬件上验证过。舵机、步进电机、I2S 音频、SPI 和 OLED 这几条路径能编译、参数校验正常、命令也有正确回应，但没有对着实物测过，因为开发时手边没有这些器件。第一次接上它们时，请当作调试，而不是当作回归。

对话保存在 PSRAM 里，实测大约每个编码后的 JSON 字符占一字节，因此上限接近 150 万 token。`agent.ctx` 默认 100 万字符，大约 25.6 万 token，最大可设到 450 万。真正值得在意的限制不是内存而是上行：每一跳都要把请求重发一遍，而这颗芯片上的 TLS 大约只能跑到 90 KB/s，所以对话很大时，每一步都会实打实地多花几秒。

## 测试环境

在 GOOUUU ESP32-S3-CAM 上开发（16 MB Flash、8 MB 八线 PSRAM、OV2640）。其它 ESP32-S3 相机板应该也能用，用 `cfg set cam.pins pwdn,reset,xclk,siod,sioc,d7,d6,d5,d4,d3,d2,d1,d0,vsync,href,pclk` 设置相机引脚即可。

## 许可

MIT，见 [LICENSE](LICENSE)。

MicroPython 以其自身的 MIT 许可一并包含，见 `components/micropython/micropython_embed/LICENSE`。
