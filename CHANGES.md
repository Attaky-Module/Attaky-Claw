# CHANGES — Attaky's modifications to esp-claw

This fork of [esp-claw](https://github.com/espressif/esp-claw)
(Apache-2.0, by Espressif Systems (Shanghai) CO LTD) carries
modifications made by Attaky. This file records those modifications
per Apache License 2.0 §4(b):

> You must cause any modified files to carry prominent notices
> stating that You changed the files;

This repository is published as **clean-history release snapshots**:
each public release on this repo is a single orphan commit, not the
ongoing internal development history. The summary below describes
the modifications relative to the upstream baseline at the time of
the snapshot.

## Themes — Attaky's contributions in this snapshot

| Theme | Areas touched |
|---|---|
| Agent Deck 1.0 board support | New board files under `application/edge_agent/boards/attaky/agent_deck_1_0/`, onboard I/O expander driver wiring, touch-controller bring-up, pinmap |
| Display arbiter + LVGL setup session | `components/common/display_arbiter/`, `components/common/setup_ui/` (LVGL port + display ownership arbitration) |
| Pure-C setup state machine + worker | `components/common/setup_ui/` (setup_sm, setup_worker — host-testable, no LVGL coupling) |
| WiFi + LLM setup flow + captive portal | `components/common/setup_ui/`, `components/common/wifi_manager/`, `application/edge_agent/components/http_server/` |
| Status cluster (emote) redesign | `components/common/emote/`, `components/claw_modules/claw_event_router/`, status_state aggregator + router observer tap |
| Offline EAF encoder + assets | `tools/eaf/`, breathing status-circle frame generator, vendored emote engine patches |
| Onboard RGB LED driver | GPIO-mode driver, LED constant-current path reserved for breathing follow-up |
| Settings → Status screen | `components/common/setup_ui/setup_screens.c`, `setup_status_format.{h,c}`, `claw_core_llm` (config summary + concurrent-safe transport snapshot), `wifi_manager` (sta_ssid) |
| Distribution rebrand to "Attaky Claw" | README, README_CN, firmware user-visible strings (mDNS / SSID defaults, LWIP hostname, LLM self-introduction, MCP tool descriptions, etc.), frontend captive-portal i18n, docs/ Astro site, branding chrome, hardware-detail scrub on public surfaces |

## Apache-2.0 §4(a) compliance

All upstream `SPDX-FileCopyrightText: ... Espressif Systems (Shanghai)
CO LTD` headers in modified files are preserved unchanged. The
`LICENSE` file at the repo root is also unmodified from upstream.

## Apache-2.0 §6 (trademark)

"ESP-Claw" remains an Espressif product name. This fork uses
"ESP-Claw" only in attribution / fork-relationship contexts (README
acknowledgements, references to upstream, Skills Lab marketplace
references). The fork ships under the distinct product name
"Attaky Claw" on Attaky hardware.

## Vendored third-party components

The `application/edge_agent/managed_components/espressif2022__esp_emote_*`
folders contain the vendored emote engine (`esp_emote_assets`,
`esp_emote_expression`), also under Apache-2.0 with
`Espressif Systems (Shanghai) CO LTD` copyright. Those source files
retain their upstream copyright headers per §4(a).

## Author

Attaky's modifications are authored under the identity:

- Name: `Attaky`
- Email: `203162318+attakygit@users.noreply.github.com`

Internal development commits live in Attaky's private repository and
are not part of this public snapshot.
