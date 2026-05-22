<div align="center">

  <a href="https://attaky.com">
    <img alt="Attaky" src="./docs/src/assets/logos/attaky-logo-horizontal.png" width="40%" />
  </a>

  <h1>Attaky Claw</h1>

  <h3>Attaky Agent Deck 1.0 的 AI 智能体固件 — fork 自 ESP-Claw</h3>

  <p>
    <a href="https://attaky.com">
      <img src="https://img.shields.io/badge/hardware-Agent_Deck_1.0-C0252B?style=flat-square" alt="Agent Deck 1.0" />
    </a>
    <a href="./LICENSE">
      <img src="https://img.shields.io/badge/license-Apache_2.0-blue?style=flat-square" alt="License" />
    </a>
    <a href="https://discord.attaky.com">
      <img src="https://img.shields.io/badge/Discord-attaky-5865F2?style=flat-square&logo=discord&logoColor=white" alt="Discord" />
    </a>
  </p>

  <a href="https://attaky.com">Attaky 主页</a>
  |
  <a href="https://attaky.com/agent-deck">Agent Deck</a>
  |
  <a href="https://discord.attaky.com">Discord</a>
  |
  <a href="./README.md">English</a>

</div>

---

**Attaky Claw** 是 [ESP-Claw](https://github.com/espressif/esp-claw)
的 fork,由 Attaky 为 Agent Deck 1.0 板卡扩展。ESP-Claw 是面向 IoT
设备的 AI Agent 框架,由 Espressif Systems (Shanghai) CO LTD
开发,采用 Apache-2.0 协议。Attaky Claw 继承同协议,加入 Attaky 的修改。

你通过对话定义设备行为;Agent Loop 在 ESP32-S3 上本地执行,决定调用
什么工具,跨重启保留上下文 —— 全在一块名片大小的板子上。

## Attaky 在 ESP-Claw 之上的扩展

- **Agent Deck 1.0 板卡支持** — 模块引脚映射、板载 I/O 扩展集成、
  外设接线
- **板载 RGB LED 驱动** — GPIO 模式驱动;LED 恒流模式
  (亮度 / 呼吸)预留接口,等后续 bring-up
- **Settings → Status 屏** — 设备本机查看 WiFi SSID、LLM
  backend 状态 + 最后调用时间、当前 IP、captive portal IP
- **Status cluster 重设计** — NORMAL / NOTIFICATION emote 状态
  接入 router observer;RECORDING / SPEAKING setter 留给未来 voice 工作
- **设备端 setup UI** — 直接在设备上完成 WiFi captive portal +
  LLM endpoint 配置,无需配套 app
- **并发安全的 LLM transport 监控** — inflight 计数 + 末次完成快照,
  并发调用下状态报告准确
- **Bring-up 修复** — emote layout 调整、label 宽度、Agent Deck 1.0
  的 sdkconfig 适配

被修改的完整文件列表可通过本 fork 的 git history 查阅。

## 硬件

Attaky Claw 为以下硬件构建并测试:

- **Agent Deck 1.0** — Attaky 参考板。ESP32-S3 平台,配彩色屏幕、
  物理按键、麦克风、喇叭、USB-C 与电池供电。完整规格详见
  [attaky.com/agent-deck](https://attaky.com/agent-deck)。

上游 ESP-Claw 也支持其他 ESP32-S3 板卡(M5Stack CoreS3、ESP32-S3
面包板等),这些在本 fork 上同样能用,但 Attaky 的上述扩展是针对
Agent Deck 1.0 的。

## 快速开始

如果你拿到的是 Agent Deck 1.0:

1. **USB-C 通电** — 板子首次开机进入 setup 模式
2. **连接屏幕上显示的 SoftAP**(开放热点,无密码)
3. **浏览器打开 `http://192.168.4.1`** — 配置 WiFi、LLM API key、
   IM bot token
4. **完成** — 板子重连你的 WiFi 上线

或者用设备端 setup:不用浏览器,直接在屏幕菜单里配 WiFi 和 LLM
endpoint。

源码编译参考 [`docs/src/content/docs/zh-cn/reference-project/build-from-source.mdx`](./docs/src/content/docs/zh-cn/reference-project/build-from-source.mdx)。
Attaky 的修改都在本仓库 `master` 分支。

## 核心特性(继承自 ESP-Claw)

以下描述的是固件本身的行为,对上游 ESP-Claw 和 Attaky Claw 都适用。

| | |
|---|---|
| **聊天造物** | IM 聊天 + Lua 动态加载 —— 不写代码就能定义设备行为 |
| **事件驱动** | 任意事件可触发 Agent Loop;毫秒级响应 |
| **结构化记忆** | 长期记忆有条理沉淀;数据留在设备本机 |
| **MCP 通讯** | 同时具备 MCP Server / Client 双重身份 |
| **开箱即用** | Board Manager + 一键烧录 |
| **组件扩展** | 模块按需裁剪;可自行加新组件 |

## 支持平台

**LLM**:OpenAI 风格 API 和 Anthropic 风格 API。已验证 GPT、Qwen
(阿里云百炼)、Claude(Anthropic)、DeepSeek。也可自定义 Endpoint。

> [!TIP]
> 自编程能力依赖模型的工具调用 + 指令遵循能力,推荐使用前沿模型。

**IM**:Telegram、QQ、飞书、微信。可扩展其他渠道。

## 文档

本仓库 [`docs/`](./docs/) 目录是完整的 Attaky Claw 文档站(Astro
Starlight),包含 tutorial、reference-core、reference-cap 三大章节。

## 反馈

- **硬件 / fork 相关问题** — Attaky Discord: [discord.attaky.com](https://discord.attaky.com)
- **框架 / 上游 bug** — 提交到 [espressif/esp-claw](https://github.com/espressif/esp-claw/issues)

## 致谢

Attaky Claw 是这条开源 agent 工作链上的最新一层。感谢:

- **[ESP-Claw](https://github.com/espressif/esp-claw)** 由 Espressif
  Systems (Shanghai) CO LTD 开发 — Apache-2.0。本 fork 扩展的框架。
- **[MimiClaw](https://github.com/memovai/mimiclaw)** 由 Ziboyan Wang
  开发 — MIT。首个在 ESP32-S3 上落地的 agent runtime,ESP-Claw
  致谢的架构参考。
- **[OpenClaw](https://github.com/openclaw/openclaw)** — MimiClaw
  和 ESP-Claw 共同追溯的概念源头。

Attaky 的贡献是 Agent Deck 1.0 硬件适配 + 上面列的扩展。Attaky 不
冒认 ESP-Claw、MimiClaw 或 OpenClaw 的作者身份,各源文件的版权归
原作者,按 file header 标注为准。

## 协议

Apache License 2.0,继承自上游 ESP-Claw。详见 [`LICENSE`](./LICENSE)。
所有源文件保留上游的版权头(Apache-2.0 §4(a) 要求);Attaky 相对上游
的修改记录见 [`CHANGES.md`](./CHANGES.md)(Apache-2.0 §4(b) 要求)。
