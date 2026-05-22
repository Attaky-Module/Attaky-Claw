<div align="center">

  <a href="https://attaky.com">
    <img alt="Attaky" src="./docs/src/assets/logos/attaky-logo-horizontal.png" width="40%" />
  </a>

  <h1>Attaky Claw</h1>

  <h3>AI agent firmware for Attaky Agent Deck 1.0, forked from ESP-Claw</h3>

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

  <a href="https://attaky.com">Attaky home</a>
  |
  <a href="https://attaky.com/agent-deck">Agent Deck</a>
  |
  <a href="https://discord.attaky.com">Discord</a>
  |
  <a href="./README_CN.md">简体中文</a>

</div>

---

**Attaky Claw** is a fork of [ESP-Claw](https://github.com/espressif/esp-claw),
extended by Attaky for the Agent Deck 1.0 board. ESP-Claw is an AI
agent framework for IoT devices, created by Espressif Systems
(Shanghai) CO LTD and licensed under Apache-2.0. Attaky Claw inherits
that license and adds Attaky's modifications under the same terms.

You define device behavior through conversation; the agent loop runs
locally on an ESP32-S3, decides what tools to call, and remembers
context across reboots — all on a board the size of a credit card.

## What Attaky added on top of ESP-Claw

- **Agent Deck 1.0 board support** — module pinmap, onboard I/O
  expander integration, peripheral wiring
- **Onboard RGB LED driver** — GPIO-mode driver, with the LED
  constant-current path reserved for brightness / breathing follow-up
  work
- **Settings → Status screen** — on-device view of WiFi SSID, LLM
  backend status + last call age, current IP, captive-portal IP
- **Status cluster redesign** — NORMAL / NOTIFICATION emote states
  wired through a router-level observer; RECORDING / SPEAKING setters
  reserved for future voice work
- **On-device setup UI** — WiFi captive portal + LLM endpoint
  configuration directly from the device, no companion app needed
- **Concurrent-safe LLM transport instrumentation** — inflight counter
  + last-completed snapshot for accurate status reporting under
  simultaneous calls
- **Various bring-up fixes** — emote layout polish, label widget
  tuning, sdkconfig adjustments for Agent Deck 1.0

The full list of files modified by Attaky lives in our fork's git
history.

## Hardware

Attaky Claw is built for and tested on:

- **Agent Deck 1.0** — Attaky's reference board. ESP32-S3 platform
  with color display, physical buttons, microphone, speaker, USB-C,
  and battery support. See [attaky.com/agent-deck](https://attaky.com/agent-deck)
  for full specifications.

Upstream ESP-Claw also supports other ESP32-S3 boards (M5Stack CoreS3,
ESP32-S3 breadboards, etc.); those work with this fork too, though
the Attaky extensions above target Agent Deck 1.0.

## Quick Start

If you have an Agent Deck 1.0:

1. **Plug in via USB-C.** The board boots into setup mode on first
   power-on.
2. **Connect to the SoftAP** shown on the device screen. It is open
   (no password).
3. **Open the setup portal** at `http://192.168.4.1`. Configure WiFi,
   your LLM API key, and your IM bot token.
4. **Done.** The board reconnects to your WiFi and goes online.

Alternatively, on-device setup: tap through the on-screen menu to
configure WiFi and the LLM endpoint without a browser.

For building from source, see [`docs/src/content/docs/en/reference-project/build-from-source.mdx`](./docs/src/content/docs/en/reference-project/build-from-source.mdx).
Attaky's modifications are tracked in the `master` branch of this
repository.

## Key Features (inherited from ESP-Claw)

These describe the firmware behavior; they apply to both upstream
ESP-Claw and Attaky Claw.

| | |
|---|---|
| **Chat as Creation** | IM chat + dynamic Lua loading — define device behavior without programming |
| **Event Driven** | Any event can trigger the Agent Loop; millisecond response |
| **Structured Memory** | Organized long-term memory; data stays on device |
| **MCP Communication** | Speaks the MCP protocol as both server and client |
| **Ready Out of the Box** | Board Manager + one-click flashing |
| **Component Extensibility** | Trim modules per project; add your own integrations |

## Supported Platforms

**LLM**: OpenAI-style and Anthropic-style APIs. Tested with GPT, Qwen
(Alibaba Cloud Bailian), Claude (Anthropic), DeepSeek. Custom
endpoints supported.

> [!TIP]
> Self-programming capability depends on the model's tool use and
> instruction-following ability. Frontier-tier models give the best
> experience.

**IM**: Telegram, QQ, Feishu, WeChat. Extendable to other channels.

## Documentation

The [`docs/`](./docs/) folder in this repository contains the full
Attaky Claw documentation site (Astro Starlight). It covers the
tutorial, reference-core, and reference-cap sections.

## Support

- **Hardware / fork-specific issues** — Attaky Discord: [discord.attaky.com](https://discord.attaky.com)
- **Framework / upstream bugs** — file at [espressif/esp-claw](https://github.com/espressif/esp-claw/issues)

## Acknowledgements

Attaky Claw is the latest layer in a chain of open-source agent work.
We thank everyone whose work made this possible:

- **[ESP-Claw](https://github.com/espressif/esp-claw)** by Espressif
  Systems (Shanghai) CO LTD — Apache-2.0. The framework this fork
  extends.
- **[MimiClaw](https://github.com/memovai/mimiclaw)** by Ziboyan Wang
  — MIT. The first agent runtime on an ESP32-S3 chip; the
  architectural reference ESP-Claw acknowledges.
- **[OpenClaw](https://github.com/openclaw/openclaw)** — the
  conceptual origin both MimiClaw and ESP-Claw trace back to.

Attaky's contribution is the Agent Deck 1.0 hardware adaptation and
the extensions listed above. Attaky does not claim authorship of
ESP-Claw, MimiClaw, or OpenClaw; their original authors retain all
source-level copyright as marked in file headers.

## License

Apache License 2.0, inherited from upstream ESP-Claw. See
[`LICENSE`](./LICENSE). All source files retain their upstream
copyright headers per Apache-2.0 §4(a); Attaky's modifications
relative to the upstream baseline are recorded in
[`CHANGES.md`](./CHANGES.md) per §4(b).
