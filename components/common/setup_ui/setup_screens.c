/*
 * setup_screens: interactive first-use WiFi screens (Task 4.2).
 *
 * WELCOME -> WIFI_SCAN -> WIFI_PW. After a real WiFi connect the flow
 * advances setup_sm to LLM and STOPS at a short "WiFi connected — LLM
 * setup next" interim screen (LLM / REVIEW / SAVE are Task 4.3),
 * returning cleanly so the no-brick session unwinds.
 *
 * Single-LVGL-task model: setup_screens_run() is invoked ONCE on the
 * LVGL task by setup_lvgl_port_run_screens(). It is the ONLY place
 * this file touches lv_*. Every blocking WiFi call runs on the
 * setup_worker task; results come back through the worker result
 * queue, drained by setup_worker_poll_results() from this function's
 * own loop (LVGL-task context — safe to touch lv_*).
 *
 * No-brick: every path logs + returns an esp_err_t. A scan/connect
 * failure never bricks (stay on screen, retry); a fatal internal
 * error returns an esp_err_t and the caller's reverse-order unwind
 * still restores emote. The driver loop is cooperatively stoppable:
 * it polls setup_lvgl_port_should_stop() every iteration and exits
 * via the shared clean-up path the moment a port teardown is
 * requested, so it can never run forever with no exit other than
 * WiFi-connect success (which would pin the owner task / app_main).
 */

#include "setup_screens.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_config.h"
#include "claw_core_llm.h"
#include "esp_err.h"
#include "esp_system.h"
#include "setup_lvgl_port.h"
#include "setup_model.h"
#include "setup_sm.h"
#include "setup_status_format.h"
#include "setup_validate.h"
#include "setup_worker.h"
#include "wifi_manager.h"


static const char *TAG = "setup_screens";

/* Bounded AP list. WPA SSID is <=32 chars +NUL = 33 (matches
 * wifi_manager_scan_record_t). 24 entries is generous for a room. */
#define SETUP_SCREENS_MAX_APS      24
/* Connect attempt budget. wifi_manager_wait_connected() blocks the
 * worker task only, never the LVGL task. */
#define SETUP_SCREENS_CONNECT_TIMEOUT_MS 20000
/* How long the Task-4.3 interim "connected" screen is shown before a
 * clean return (LLM screen is a later task). */
#define SETUP_SCREENS_INTERIM_MS   2500
/* LVGL loop step. Mirrors the cadence used by the built-in patterns
 * in setup_lvgl_port.c. */
#define SETUP_SCREENS_STEP_MS      20

/* Worker job kinds (echoed back via setup_worker_result_t.code). */
typedef enum {
    SCR_JOB_SCAN = 1,
    SCR_JOB_CONNECT,
} scr_job_kind_t;

struct setup_screens_ctx {
    setup_sm_mode_t mode;
    setup_sm_state_t state;

    /* Validated fields (only wifi_ssid/wifi_password used in 4.2).
     * setup_validate_wifi() reads this; the SSID/password buffers
     * handed to the worker live here so they outlive the job. */
    setup_fields_t fields;

    /* Scan results owned by this heap ctx (outlive the scan job). */
    wifi_manager_scan_record_t aps[SETUP_SCREENS_MAX_APS];
    uint16_t ap_count;

    /* wifi_manager_config_t points at fields.wifi_ssid /
     * fields.wifi_password; both live in this heap ctx so they
     * outlive the connect job. */
    wifi_manager_config_t wcfg;

    /* Worker job completion. The ONLY cross-task ordering point is the
     * setup_worker result QUEUE: the worker writes aps[]/ap_count
     * BEFORE setup_worker_post_result(), and scr_result_cb() (which
     * runs on the LVGL task during setup_worker_poll_results(), AFTER
     * the queue receive) is what marks the job complete. The queue
     * receive is the memory barrier that makes the worker's aps[] /
     * ap_count / status writes visible to the LVGL task — so these
     * fields are NOT volatile and are written/read only on the LVGL
     * task (the callback and the loop). No bare volatile flag is used
     * as a parallel completion signal; the queue is the single one.
     * job_running is set/cleared only on the LVGL task (job submitted
     * vs completion observed) and gates UI re-entry. */
    bool job_running;
    bool job_done;
    esp_err_t job_status;
    int job_kind;

    /* UI intents set by LVGL event callbacks (run on the LVGL task,
     * same task as the loop — no lock needed). */
    bool ev_start;        /* WELCOME: Begin tapped */
    bool ev_rescan;       /* WIFI_SCAN: Rescan tapped */
    bool ev_manual;       /* WIFI_SCAN: Manual SSID tapped */
    bool ev_pick;         /* WIFI_SCAN: an AP row tapped */
    int  ev_pick_idx;     /* index into aps[] (or -1 = manual) */
    bool ev_connect;      /* WIFI_PW: Connect tapped */
    bool ev_back;         /* WIFI_PW / LLM / REVIEW: Back tapped */
    bool ev_show_pw;      /* WIFI_PW: show/hide toggled */
    bool ev_llm_backend;  /* LLM: backend dropdown selection changed */
    bool ev_next;         /* LLM: Next tapped */
    bool ev_save;         /* REVIEW: Save tapped */
    bool ev_menu_wifi;    /* MENU (settings): Wi-Fi tapped */
    bool ev_menu_llm;     /* MENU (settings): LLM tapped */
    bool ev_menu_status;  /* MENU (settings): Status tapped */

    /* Live widget handles for the current screen (NULL when torn
     * down). Owned by the active LVGL screen; cleaned via
     * lv_obj_clean() at teardown. */
    lv_obj_t *kb;
    lv_obj_t *pw_ta;
    lv_obj_t *ssid_ta;       /* manual-SSID textarea (NULL if AP pick) */
    lv_obj_t *err_label;
    lv_obj_t *spinner;
    bool pw_hidden;

    /* I1: last backend whose defaults were applied via
     * scr_llm_apply_backend_defaults(). SETUP_BACKEND_COUNT = none yet
     * (invalid sentinel — valid backends are 0..COUNT-1). Guards
     * against a spurious/programmatic dropdown VALUE_CHANGED (e.g.
     * lv_dropdown_set_selected on REVIEW->Back) clobbering restored
     * user edits with backend defaults. */
    setup_backend_t llm_applied_backend;

    /* LLM screen widget handles (NULL when torn down). */
    lv_obj_t *llm_backend_dd;  /* backend lv_dropdown */
    lv_obj_t *llm_base_url_ta; /* editable base_url textarea */
    lv_obj_t *llm_model_ta;    /* editable model textarea */
    lv_obj_t *llm_api_key_ta;  /* masked api_key textarea */

    /* STATUS screen widget handles (NULL when torn down). The 5 value
     * labels are stowed at build time so scr_status_refresh() can update
     * them in place at 0.2 Hz without rebuilding the screen. */
    lv_obj_t *status_lbl_ssid;        /* Wi-Fi SSID value */
    lv_obj_t *status_lbl_llm;         /* "<backend> · <model>" value */
    lv_obj_t *status_lbl_llm_status;  /* color-coded last-call line */
    lv_obj_t *status_lbl_ip;          /* Device IP value */
    lv_obj_t *status_lbl_portal;      /* Portal IP value */

    /* STATUS 0.2 Hz tick deadline tracker (esp_timer microseconds).
     * Compared in the dispatch case against a 5,000,000 us interval. */
    int64_t status_last_refresh_us;
};

/* ---- worker jobs (run on the worker task; NO lv_* here) ---------- */

static void scr_scan_job(void *arg)
{
    setup_screens_ctx_t *c = (setup_screens_ctx_t *)arg;
    uint16_t n = 0;
    esp_err_t err = wifi_manager_scan_aps(c->aps, SETUP_SCREENS_MAX_APS,
                                          &n);
    /* aps[]/ap_count are written HERE on the worker task, then
     * published to the LVGL task by the result-queue post below: the
     * LVGL task's queue receive in setup_worker_poll_results() is the
     * barrier that makes these writes visible. Do NOT set any bare
     * completion flag here — scr_result_cb() (LVGL task, post-receive)
     * is the single completion signal. */
    c->ap_count = (err == ESP_OK) ? n : 0;
    setup_worker_result_t r = {
        .status = err, .code = SCR_JOB_SCAN, .arg = c,
    };
    setup_worker_post_result(&r);
}

static void scr_connect_job(void *arg)
{
    setup_screens_ctx_t *c = (setup_screens_ctx_t *)arg;
    /* cfg buffers (fields.wifi_ssid / wifi_password) live in the heap
     * ctx, so they outlive this job. */
    c->wcfg.sta_ssid = c->fields.wifi_ssid;
    c->wcfg.sta_password = c->fields.wifi_password;
    esp_err_t err = wifi_manager_apply_sta_config(&c->wcfg);
    if (err == ESP_OK) {
        err = wifi_manager_wait_connected(
            SETUP_SCREENS_CONNECT_TIMEOUT_MS);
    }
    /* Status travels in the result record (queue-ordered). No bare
     * completion flag here — scr_result_cb() on the LVGL task marks
     * completion after the queue receive (the cross-core barrier). */
    setup_worker_result_t r = {
        .status = err, .code = SCR_JOB_CONNECT, .arg = c,
    };
    setup_worker_post_result(&r);
}

/* Result sink — runs on the LVGL task during setup_worker_poll_results()
 * (called from scr_pump() in this function's own loop). This callback
 * fires AFTER the result-queue receive, which is the cross-task /
 * cross-core ordering point: by the time we are here the worker's
 * aps[]/ap_count writes (done before setup_worker_post_result()) are
 * visible to this task. This is the SINGLE place a job is marked
 * complete — there is no parallel bare-volatile completion flag. The
 * job is correlated via r->arg (the ctx the job was submitted with)
 * and r->code (SCR_JOB_SCAN vs SCR_JOB_CONNECT), so the loop knows
 * which job finished. Runs on the LVGL task; touching c-> here is
 * single-task safe (same task as the loop). */
static void scr_result_cb(const setup_worker_result_t *r, void *cbctx)
{
    if (r == NULL) {
        return;
    }
    setup_screens_ctx_t *c = (setup_screens_ctx_t *)r->arg;
    /* 4.2-M4 invariant: jobs are always submitted with the same ctx the
     * result cb was registered with, so r->arg must equal cbctx. A
     * mismatch means a stale/cross job — log it (non-fatal diagnostic). */
    if (cbctx != NULL && r->arg != cbctx) {
        ESP_LOGW(TAG, "result arg %p != cbctx %p (stale job?)",
                 r->arg, cbctx);
    }
    ESP_LOGI(TAG, "worker result kind=%d status=%s",
             r->code, esp_err_to_name(r->status));
    if (c == NULL) {
        return;
    }
    c->job_kind = r->code;
    c->job_status = r->status;
    c->job_done = true;
}

/* ---- LVGL event callbacks (run on the LVGL task) ---------------- */

static void scr_evt_set_bool(lv_event_t *e)
{
    bool *flag = (bool *)lv_event_get_user_data(e);
    if (flag != NULL) {
        *flag = true;
    }
}

static void scr_evt_pick_ap(lv_event_t *e)
{
    setup_screens_ctx_t *c =
        (setup_screens_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target_obj(e);
    if (c == NULL || btn == NULL) {
        return;
    }
    /* The aps[] index was baked into the button at build time via
     * lv_obj_set_user_data. It is stowed as (index + 1) so a valid
     * index 0 is distinguishable from unset/NULL user_data: a 0 here
     * means "no index stowed", not "first AP". Recover with idx - 1. */
    intptr_t raw = (intptr_t)lv_obj_get_user_data(btn);
    if (raw <= 0) {
        ESP_LOGW(TAG, "pick: row has no stowed index — ignored");
        return;
    }
    c->ev_pick_idx = (int)(raw - 1);
    c->ev_pick = true;
}

/* ---- screen builders (LVGL task) -------------------------------- */

static void scr_clear_screen(setup_screens_ctx_t *c)
{
    lv_obj_clean(lv_screen_active());
    c->kb = NULL;
    c->pw_ta = NULL;
    c->ssid_ta = NULL;
    c->err_label = NULL;
    c->spinner = NULL;
    c->llm_backend_dd = NULL;
    c->llm_base_url_ta = NULL;
    c->llm_model_ta = NULL;
    c->llm_api_key_ta = NULL;
    c->status_lbl_ssid = NULL;
    c->status_lbl_llm = NULL;
    c->status_lbl_llm_status = NULL;
    c->status_lbl_ip = NULL;
    c->status_lbl_portal = NULL;
}

static void scr_style_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_make(0x10, 0x18, 0x28), 0);
}

static void scr_build_welcome(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Welcome");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Let's connect to WiFi");
    lv_obj_set_style_text_color(sub, lv_color_make(0xC0, 0xC8, 0xD0),
                                0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 52);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 56);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_event_cb(btn, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_start);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Begin");
    lv_obj_center(bl);
}

/* SETTINGS hub (no WELCOME in settings mode). Four intents wired with
 * the same scr_evt_set_bool + c->ev_* idiom as the other screens: Wi-Fi
 * opens the WIFI_SCAN subflow, LLM opens the LLM subflow, Status opens
 * the STATUS read-only screen, Back leaves settings (SM MENU + BACK ->
 * EXIT -> clean unwind, emote resumes). Layout per design §3.3 / §5.2:
 * four 50-px buttons at y = -66 / -10 / 46 / 102 (16 px gaps). */
static void scr_build_menu(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *wifi = lv_button_create(scr);
    lv_obj_set_size(wifi, 180, 50);
    lv_obj_align(wifi, LV_ALIGN_CENTER, 0, -66);
    lv_obj_add_event_cb(wifi, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_menu_wifi);
    lv_obj_t *wl = lv_label_create(wifi);
    lv_label_set_text(wl, "Wi-Fi");
    lv_obj_center(wl);

    lv_obj_t *llm = lv_button_create(scr);
    lv_obj_set_size(llm, 180, 50);
    lv_obj_align(llm, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_event_cb(llm, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_menu_llm);
    lv_obj_t *ll = lv_label_create(llm);
    lv_label_set_text(ll, "LLM");
    lv_obj_center(ll);

    lv_obj_t *status = lv_button_create(scr);
    lv_obj_set_size(status, 180, 50);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 46);
    lv_obj_add_event_cb(status, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_menu_status);
    lv_obj_t *sl = lv_label_create(status);
    lv_label_set_text(sl, "Status");
    lv_obj_center(sl);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 180, 50);
    lv_obj_align(back, LV_ALIGN_CENTER, 0, 102);
    lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_back);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);
}

/* STATUS screen colour palette per design §5.4. dim = field labels +
 * "not configured" / placeholder text; green = healthy last call; red =
 * portal-down or last-call error; white = live value. */
#define SCR_STATUS_COL_DIM    lv_color_make(0xC0, 0xC8, 0xD0)
#define SCR_STATUS_COL_GREEN  lv_color_make(0x50, 0xE0, 0x80)
#define SCR_STATUS_COL_RED    lv_color_make(0xE0, 0x60, 0x60)

/* Settings -> STATUS (read-only). Layout per design §5.1-§5.4. Build is
 * pure layout: it allocates 5 value/status labels and stows them in
 * c->status_lbl_* with placeholder text, so the very first refresh
 * (immediately after build, then every 5 s in the dispatch case) fills
 * the real snapshot data. No data fetch happens here — keeps build
 * cheap and refresh path single. Default Montserrat font for every
 * label; no new fonts pulled in. */
static void scr_build_status(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Device Status");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 80, 38);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_back);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    /* Row layout (v1 1-row label+value style, with LLM status pushed
     * down one slot so a long backend/model value can't visually
     * crowd it):
     *   y=44   Wi-Fi     | <ssid>           (LONG_DOT, width 220)
     *   y=72   LLM       | <backend> - <model>  (LONG_DOT, width 220)
     *   y=100            | <status line>    (color-coded, x=120)
     *   y=130  Device IP | <ip>
     *   y=158  Portal IP | <ip>
     * Field labels at x=8, value/status at x=120. LONG_DOT width=220
     * starts at x=120 so the label thinks it has horizontal room past
     * the visible screen edge — keeps text from wrapping; visually
     * the right edge is clipped by the screen. Long values still get
     * the "..." truncation when their natural width exceeds 220 px. */

    /* Row 1: Wi-Fi. */
    lv_obj_t *wf_lbl = lv_label_create(scr);
    lv_label_set_text(wf_lbl, "Wi-Fi");
    lv_obj_set_style_text_color(wf_lbl, SCR_STATUS_COL_DIM, 0);
    lv_obj_align(wf_lbl, LV_ALIGN_TOP_LEFT, 8, 44);

    lv_obj_t *ssid_v = lv_label_create(scr);
    lv_label_set_long_mode(ssid_v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssid_v, 220);
    lv_label_set_text(ssid_v, "");
    lv_obj_set_style_text_color(ssid_v, lv_color_white(), 0);
    lv_obj_align(ssid_v, LV_ALIGN_TOP_LEFT, 80, 44);
    c->status_lbl_ssid = ssid_v;

    /* Row 2: LLM value (same row as field-label) + status one row below. */
    lv_obj_t *llm_fld = lv_label_create(scr);
    lv_label_set_text(llm_fld, "LLM");
    lv_obj_set_style_text_color(llm_fld, SCR_STATUS_COL_DIM, 0);
    lv_obj_align(llm_fld, LV_ALIGN_TOP_LEFT, 8, 72);

    lv_obj_t *llm_v = lv_label_create(scr);
    lv_label_set_long_mode(llm_v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(llm_v, 220);
    lv_label_set_text(llm_v, "");
    lv_obj_set_style_text_color(llm_v, lv_color_white(), 0);
    lv_obj_align(llm_v, LV_ALIGN_TOP_LEFT, 80, 72);
    c->status_lbl_llm = llm_v;

    lv_obj_t *llm_s = lv_label_create(scr);
    lv_label_set_long_mode(llm_s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(llm_s, 220);
    lv_label_set_text(llm_s, "");
    lv_obj_set_style_text_color(llm_s, SCR_STATUS_COL_DIM, 0);
    /* y=100: one row (~28 px) below the LLM value line so a long
     * backend/model string can't visually crowd the status. */
    lv_obj_align(llm_s, LV_ALIGN_TOP_LEFT, 80, 100);
    c->status_lbl_llm_status = llm_s;

    /* Row 3: Device IP (shifted +4 from the v1 y=126 to preserve the
     * 28-30 px section gap after pushing LLM status to y=100). */
    lv_obj_t *ip_lbl = lv_label_create(scr);
    lv_label_set_text(ip_lbl, "Device IP");
    lv_obj_set_style_text_color(ip_lbl, SCR_STATUS_COL_DIM, 0);
    lv_obj_align(ip_lbl, LV_ALIGN_TOP_LEFT, 8, 130);

    lv_obj_t *ip_v = lv_label_create(scr);
    lv_label_set_text(ip_v, "");
    lv_obj_set_style_text_color(ip_v, lv_color_white(), 0);
    lv_obj_align(ip_v, LV_ALIGN_TOP_LEFT, 80, 130);
    c->status_lbl_ip = ip_v;

    /* Row 4: Portal IP. */
    lv_obj_t *po_lbl = lv_label_create(scr);
    lv_label_set_text(po_lbl, "Portal IP");
    lv_obj_set_style_text_color(po_lbl, SCR_STATUS_COL_DIM, 0);
    lv_obj_align(po_lbl, LV_ALIGN_TOP_LEFT, 8, 158);

    lv_obj_t *po_v = lv_label_create(scr);
    lv_label_set_text(po_v, "");
    lv_obj_set_style_text_color(po_v, lv_color_white(), 0);
    lv_obj_align(po_v, LV_ALIGN_TOP_LEFT, 80, 158);
    c->status_lbl_portal = po_v;
}

/* Snapshot fetch + label re-text. Runs on the LVGL task at 0.2 Hz from
 * the SETUP_ST_STATUS dispatch case (plus once immediately after build).
 * Pure read-only: every snapshot API is a copy-out, so this is safe to
 * call while the rest of the system runs. Format rules per design §6. */
static void scr_status_refresh(setup_screens_ctx_t *c)
{
    if (c == NULL) {
        return;
    }

    wifi_manager_status_t wifi;
    memset(&wifi, 0, sizeof(wifi));
    wifi_manager_get_status(&wifi);

    claw_core_llm_last_call_t lc;
    memset(&lc, 0, sizeof(lc));
    claw_core_llm_get_last_call(&lc);

    claw_core_llm_config_summary_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    claw_core_llm_get_config_summary(&cfg);

    /* §6.1 Wi-Fi SSID value. */
    if (c->status_lbl_ssid != NULL) {
        if (wifi.sta_connected && wifi.sta_ssid != NULL &&
            wifi.sta_ssid[0] != '\0') {
            lv_label_set_text(c->status_lbl_ssid, wifi.sta_ssid);
            lv_obj_set_style_text_color(c->status_lbl_ssid,
                                        lv_color_white(), 0);
        } else if (wifi.sta_connected) {
            lv_label_set_text(c->status_lbl_ssid, "(connected)");
            lv_obj_set_style_text_color(c->status_lbl_ssid,
                                        SCR_STATUS_COL_DIM, 0);
        } else if (wifi.sta_configured) {
            lv_label_set_text(c->status_lbl_ssid, "-");
            lv_obj_set_style_text_color(c->status_lbl_ssid,
                                        SCR_STATUS_COL_DIM, 0);
        } else {
            lv_label_set_text(c->status_lbl_ssid, "(not configured)");
            lv_obj_set_style_text_color(c->status_lbl_ssid,
                                        SCR_STATUS_COL_DIM, 0);
        }
    }

    /* §6.1 Device IP value. */
    if (c->status_lbl_ip != NULL) {
        if (wifi.sta_connected && wifi.sta_ip != NULL &&
            wifi.sta_ip[0] != '\0') {
            lv_label_set_text(c->status_lbl_ip, wifi.sta_ip);
            lv_obj_set_style_text_color(c->status_lbl_ip,
                                        lv_color_white(), 0);
        } else {
            lv_label_set_text(c->status_lbl_ip, "-");
            lv_obj_set_style_text_color(c->status_lbl_ip,
                                        SCR_STATUS_COL_DIM, 0);
        }
    }

    /* §6.2 Portal IP value (AP). */
    if (c->status_lbl_portal != NULL) {
        if (wifi.ap_active && wifi.ap_ip != NULL &&
            wifi.ap_ip[0] != '\0') {
            lv_label_set_text(c->status_lbl_portal, wifi.ap_ip);
            lv_obj_set_style_text_color(c->status_lbl_portal,
                                        lv_color_white(), 0);
        } else {
            lv_label_set_text(c->status_lbl_portal, "(AP down)");
            lv_obj_set_style_text_color(c->status_lbl_portal,
                                        SCR_STATUS_COL_RED, 0);
        }
    }

    /* §6.3 LLM value line — first match wins, top to bottom. The
     * "(LLM not initialized)" rail catches the boot-not-done case so a
     * blank backend/model never reads as "all fine, just unknown". */
    if (c->status_lbl_llm != NULL) {
        if (!cfg.initialized) {
            lv_label_set_text(c->status_lbl_llm,
                              "(LLM not initialized)");
            lv_obj_set_style_text_color(c->status_lbl_llm,
                                        SCR_STATUS_COL_DIM, 0);
        } else if (cfg.backend_type[0] == '\0') {
            lv_label_set_text(c->status_lbl_llm, "(unknown backend)");
            lv_obj_set_style_text_color(c->status_lbl_llm,
                                        SCR_STATUS_COL_DIM, 0);
        } else if (cfg.model[0] == '\0') {
            lv_label_set_text(c->status_lbl_llm, cfg.backend_type);
            lv_obj_set_style_text_color(c->status_lbl_llm,
                                        lv_color_white(), 0);
        } else {
            char buf[128];
            /* ASCII separator " - " (NOT U+00B7 "·"): default LVGL
             * Montserrat doesn't ship the middle-dot glyph, and
             * pulling in an extended font for one separator would
             * cost flash. Plan §5.4 explicitly allows this ASCII
             * fallback. */
            snprintf(buf, sizeof(buf), "%s - %s",
                     cfg.backend_type, cfg.model);
            lv_label_set_text(c->status_lbl_llm, buf);
            lv_obj_set_style_text_color(c->status_lbl_llm,
                                        lv_color_white(), 0);
        }
    }

    /* §6.4 LLM status line. format_call_age() handles negative deltas
     * (clamps to "just now"), so a clock skew between the recorded
     * timestamp and now never crashes the formatter. ASCII "OK" / "ERR"
     * / "..." fall back for the same Montserrat-no-symbol reason as
     * the value-line separator above; "calling..." stays as three
     * ASCII dots. */
    if (c->status_lbl_llm_status != NULL) {
        switch (lc.status) {
        case CLAW_CORE_LLM_LAST_CALL_NONE:
            lv_label_set_text(c->status_lbl_llm_status, "(no calls yet)");
            lv_obj_set_style_text_color(c->status_lbl_llm_status,
                                        SCR_STATUS_COL_DIM, 0);
            break;
        case CLAW_CORE_LLM_LAST_CALL_IN_FLIGHT:
            lv_label_set_text(c->status_lbl_llm_status, "calling...");
            lv_obj_set_style_text_color(c->status_lbl_llm_status,
                                        SCR_STATUS_COL_DIM, 0);
            break;
        case CLAW_CORE_LLM_LAST_CALL_OK: {
            char age[16];
            int64_t delta = esp_timer_get_time() - lc.timestamp_us;
            if (format_call_age(delta, age, sizeof(age)) == NULL) {
                strlcpy(age, "?", sizeof(age));
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "OK last call %s", age);
            lv_label_set_text(c->status_lbl_llm_status, buf);
            lv_obj_set_style_text_color(c->status_lbl_llm_status,
                                        SCR_STATUS_COL_GREEN, 0);
            break;
        }
        case CLAW_CORE_LLM_LAST_CALL_ERROR: {
            char age[16];
            int64_t delta = esp_timer_get_time() - lc.timestamp_us;
            if (format_call_age(delta, age, sizeof(age)) == NULL) {
                strlcpy(age, "?", sizeof(age));
            }
            char buf[96];
            /* lc.msg is a fixed 48-byte buffer; treat empty as "(err)"
             * so the status line is never a bare "ERR  Δ". */
            const char *msg = (lc.msg[0] != '\0') ? lc.msg : "(err)";
            snprintf(buf, sizeof(buf), "ERR %s %s", msg, age);
            lv_label_set_text(c->status_lbl_llm_status, buf);
            lv_obj_set_style_text_color(c->status_lbl_llm_status,
                                        SCR_STATUS_COL_RED, 0);
            break;
        }
        default:
            lv_label_set_text(c->status_lbl_llm_status, "");
            lv_obj_set_style_text_color(c->status_lbl_llm_status,
                                        SCR_STATUS_COL_DIM, 0);
            break;
        }
    }
}

static void scr_build_wifi_scan(setup_screens_ctx_t *c, bool scanning)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Choose WiFi");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* SETTINGS only: a Back affordance so the WiFi subflow is escapable
     * (SM edge WIFI_SCAN + SETUP_EV_BACK -> MENU exists for !first_use but
     * was unreachable with no UI control). FIRST_USE has no such SM edge
     * and MUST stay unchanged — no button is added there. Same
     * scr_evt_set_bool -> c->ev_back idiom as every other Back button;
     * placed top-right so it does not disturb the bottom Rescan/Manual
     * action row or the AP list. Shown in both the scanning and list
     * views so a stuck scan is still cancellable. */
    if (c->mode == SETUP_MODE_SETTINGS) {
        lv_obj_t *back = lv_button_create(scr);
        lv_obj_set_size(back, 84, 38);
        lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -6, 4);
        lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                            &c->ev_back);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, "Back");
        lv_obj_center(bl);
    }

    if (scanning) {
        lv_obj_t *sp = lv_spinner_create(scr);
        lv_obj_set_size(sp, 56, 56);
        lv_obj_center(sp);
        c->spinner = sp;
        lv_obj_t *msg = lv_label_create(scr);
        lv_label_set_text(msg, "Scanning...");
        lv_obj_set_style_text_color(msg,
                                    lv_color_make(0xC0, 0xC8, 0xD0),
                                    0);
        lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -16);
        return;
    }

    /* Bottom action row: Rescan + Manual SSID. */
    lv_obj_t *rescan = lv_button_create(scr);
    lv_obj_set_size(rescan, 110, 44);
    lv_obj_align(rescan, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_add_event_cb(rescan, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_rescan);
    lv_obj_t *rl = lv_label_create(rescan);
    lv_label_set_text(rl, "Rescan");
    lv_obj_center(rl);

    lv_obj_t *manual = lv_button_create(scr);
    lv_obj_set_size(manual, 140, 44);
    lv_obj_align(manual, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_add_event_cb(manual, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_manual);
    lv_obj_t *ml = lv_label_create(manual);
    lv_label_set_text(ml, "Manual SSID");
    lv_obj_center(ml);

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(70));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 30);

    if (c->ap_count == 0) {
        lv_obj_t *none = lv_label_create(list);
        lv_label_set_text(none, "No networks found");
        lv_obj_set_style_text_color(none,
                                    lv_color_make(0xC0, 0xC8, 0xD0),
                                    0);
        return;
    }

    for (uint16_t i = 0; i < c->ap_count; i++) {
        const char *ssid = c->aps[i].ssid;
        if (ssid[0] == '\0') {
            continue; /* skip hidden/blank SSIDs in the visible list */
        }
        lv_obj_t *row = lv_list_add_button(list, LV_SYMBOL_WIFI, ssid);
        /* Stow the aps[] index on the row so the pick callback maps
         * back without relying on child order. Store (i + 1) so a
         * valid index 0 is not indistinguishable from unset/NULL
         * user_data; the pick callback recovers it as idx - 1. */
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(row, scr_evt_pick_ap, LV_EVENT_CLICKED,
                            c);
    }
}

static void scr_build_manual_ssid(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hidden network SSID");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "SSID");
    lv_obj_set_width(ta, LV_PCT(92));
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 34);
    c->ssid_ta = ta;
    /* SETTINGS (production): show the current SSID from the seeded
     * c->fields so the user edits real config, not a blank. */
    if (c->mode == SETUP_MODE_SETTINGS &&
        c->fields.wifi_ssid[0] != '\0') {
        lv_textarea_set_text(ta, c->fields.wifi_ssid);
    }

    lv_obj_t *ok = lv_button_create(scr);
    lv_obj_set_size(ok, 120, 40);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -6, 78);
    lv_obj_add_event_cb(ok, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_pick);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, "Next");
    lv_obj_center(okl);

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(50));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    c->kb = kb;
}

static void scr_build_wifi_pw(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();
    c->pw_hidden = true;

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, c->fields.wifi_ssid);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 4);

    lv_obj_t *ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_width(ta, LV_PCT(70));
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 6, 26);
    c->pw_ta = ta;
    /* SETTINGS (production): the title already shows the current SSID
     * (c->fields.wifi_ssid, seeded from the draft). Prefill the password
     * from the seeded c->fields.wifi_password if non-empty so the user
     * sees+edits the real config. Reachable in SETTINGS only via the
     * WiFi subflow Back-to-PW edge (AP-pick clears wifi_password
     * first, so a c->fields-gated prefill would not fire there). */
    if (c->mode == SETUP_MODE_SETTINGS &&
        c->fields.wifi_password[0] != '\0') {
        lv_textarea_set_text(ta, c->fields.wifi_password);
    }

    lv_obj_t *eye = lv_button_create(scr);
    lv_obj_set_size(eye, 56, 38);
    lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -6, 26);
    lv_obj_add_event_cb(eye, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_show_pw);
    lv_obj_t *el = lv_label_create(eye);
    lv_label_set_text(el, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(el);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 84, 38);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 66);
    lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_back);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    lv_obj_t *conn = lv_button_create(scr);
    lv_obj_set_size(conn, 120, 38);
    lv_obj_align(conn, LV_ALIGN_TOP_RIGHT, -6, 66);
    lv_obj_add_event_cb(conn, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_connect);
    lv_obj_t *cl = lv_label_create(conn);
    lv_label_set_text(cl, "Connect");
    lv_obj_center(cl);

    lv_obj_t *err = lv_label_create(scr);
    lv_label_set_text(err, "");
    lv_obj_set_style_text_color(err, lv_color_make(0xFF, 0x60, 0x60),
                                0);
    lv_obj_set_width(err, LV_PCT(92));
    lv_obj_align(err, LV_ALIGN_TOP_LEFT, 6, 104);
    c->err_label = err;

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(50));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    c->kb = kb;
}

static void scr_build_interim(setup_screens_ctx_t *c, const char *msg)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_make(0x0E, 0x2A, 0x16), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_width(label, LV_PCT(90));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
}

/* Map a dropdown selection index (option order OPENAI/ANTHROPIC/CUSTOM,
 * built from setup_backend_label below) to a setup_backend_t. Bounded so
 * an out-of-range index can never index past SETUP_BACKEND_COUNT. */
static setup_backend_t scr_backend_for_sel(uint32_t sel)
{
    if (sel >= (uint32_t)SETUP_BACKEND_COUNT) {
        return SETUP_BACKEND_CUSTOM;
    }
    return (setup_backend_t)sel;
}

/* Prefill base_url/model from setup_backend_defaults() for the backend
 * currently selected in the dropdown. setup_backend_defaults() ALWAYS
 * writes both buffers (empty string for CUSTOM), so the textareas always
 * reflect the chosen backend; they remain user-editable afterwards. */
static void scr_llm_apply_backend_defaults(setup_screens_ctx_t *c)
{
    if (c->llm_backend_dd == NULL) {
        return;
    }
    uint32_t sel = lv_dropdown_get_selected(c->llm_backend_dd);
    setup_backend_t b = scr_backend_for_sel(sel);

    /* I1: only prefill when the selection is a DIFFERENT backend than
     * the last one we applied. A genuine user backend change
     * intentionally replaces base_url/model with that backend's
     * defaults (spec: "pre-fill … (editable)"); a spurious/programmatic
     * re-seat (e.g. lv_dropdown_set_selected on REVIEW->Back) selects
     * the same backend and is suppressed so it cannot clobber the
     * just-restored user edits. */
    if (b == c->llm_applied_backend) {
        return;
    }
    c->llm_applied_backend = b;

    char bu[SETUP_STR_LEN];
    char mo[SETUP_MODEL_LEN];
    setup_backend_defaults(b, bu, sizeof(bu), mo, sizeof(mo));

    if (c->llm_base_url_ta != NULL) {
        lv_textarea_set_text(c->llm_base_url_ta, bu);
    }
    if (c->llm_model_ta != NULL) {
        lv_textarea_set_text(c->llm_model_ta, mo);
    }
}

static void scr_build_llm(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LLM backend");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 2);

    /* Backend dropdown — options are the friendly LABELS (UI text), in
     * setup_backend_t order so the selection index maps 1:1 to the enum
     * via scr_backend_for_sel(). The PERSISTED value is the claw id from
     * setup_backend_id(), written into c->fields.llm_backend_type on
     * Next (NOT the label). */
    lv_obj_t *dd = lv_dropdown_create(scr);
    /* I3: sized so the label option list cannot silently truncate, which
     * would corrupt the index<->enum invariant relied on by
     * scr_backend_for_sel(). 48 bytes/label is generous for the longest
     * label ("Custom (OpenAI-compatible)" = 26); +1 per label for the
     * '\n' separator / final NUL. */
    char opts[SETUP_BACKEND_COUNT * (48 + 1)];
    int n = snprintf(opts, sizeof(opts), "%s\n%s\n%s",
                     setup_backend_label(SETUP_BACKEND_OPENAI),
                     setup_backend_label(SETUP_BACKEND_ANTHROPIC),
                     setup_backend_label(SETUP_BACKEND_CUSTOM));
    if (n < 0 || (size_t)n >= sizeof(opts)) {
        /* Fail-soft: no-brick discipline — proceed with whatever fit
         * (NUL-terminated by snprintf) but log loudly so the contract
         * violation is visible. Do NOT abort/ESP_ERROR_CHECK. */
        ESP_LOGE(TAG,
                 "backend opts truncated (need %d, have %u) — "
                 "index<->enum map may be wrong",
                 n, (unsigned)sizeof(opts));
    }
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_width(dd, LV_PCT(60));
    lv_obj_align(dd, LV_ALIGN_TOP_LEFT, 6, 24);
    lv_obj_add_event_cb(dd, scr_evt_set_bool, LV_EVENT_VALUE_CHANGED,
                        &c->ev_llm_backend);
    c->llm_backend_dd = dd;

    lv_obj_t *bu_lbl = lv_label_create(scr);
    lv_label_set_text(bu_lbl, "Base URL");
    lv_obj_set_style_text_color(bu_lbl, lv_color_make(0xC0, 0xC8, 0xD0),
                                0);
    lv_obj_align(bu_lbl, LV_ALIGN_TOP_LEFT, 6, 60);
    lv_obj_t *bu = lv_textarea_create(scr);
    lv_textarea_set_one_line(bu, true);
    lv_textarea_set_placeholder_text(bu, "https://...");
    lv_obj_set_width(bu, LV_PCT(92));
    lv_obj_align(bu, LV_ALIGN_TOP_LEFT, 6, 78);
    c->llm_base_url_ta = bu;

    lv_obj_t *mo_lbl = lv_label_create(scr);
    lv_label_set_text(mo_lbl, "Model");
    lv_obj_set_style_text_color(mo_lbl, lv_color_make(0xC0, 0xC8, 0xD0),
                                0);
    lv_obj_align(mo_lbl, LV_ALIGN_TOP_LEFT, 6, 110);
    lv_obj_t *mo = lv_textarea_create(scr);
    lv_textarea_set_one_line(mo, true);
    lv_textarea_set_placeholder_text(mo, "model name");
    lv_obj_set_width(mo, LV_PCT(92));
    lv_obj_align(mo, LV_ALIGN_TOP_LEFT, 6, 128);
    c->llm_model_ta = mo;

    lv_obj_t *ak_lbl = lv_label_create(scr);
    lv_label_set_text(ak_lbl, "API key");
    lv_obj_set_style_text_color(ak_lbl, lv_color_make(0xC0, 0xC8, 0xD0),
                                0);
    lv_obj_align(ak_lbl, LV_ALIGN_TOP_LEFT, 6, 160);
    lv_obj_t *ak = lv_textarea_create(scr);
    lv_textarea_set_one_line(ak, true);
    lv_textarea_set_password_mode(ak, true); /* masked entry */
    lv_textarea_set_placeholder_text(ak, "sk-...");
    lv_obj_set_width(ak, LV_PCT(92));
    lv_obj_align(ak, LV_ALIGN_TOP_LEFT, 6, 178);
    c->llm_api_key_ta = ak;
    /* SETTINGS (production): show the current LLM config from the seeded
     * c->fields, never backend blank-defaults. Map the persisted claw id
     * back to a setup_backend_t (same inverse helper used by REVIEW->Back)
     * and seat the dropdown; on no exact match leave the default selection
     * and do not crash. Mark llm_applied_backend = the seated selection so
     * scr_llm_apply_backend_defaults() (called below) is suppressed (same
     * backend) and does NOT clobber the seeded c->fields base_url/model. A
     * genuine later user dropdown change still applies that backend's
     * defaults (b != applied). */
    if (c->mode == SETUP_MODE_SETTINGS) {
        setup_backend_t bsel = SETUP_BACKEND_CUSTOM;
        if (setup_backend_from_id(c->fields.llm_backend_type, &bsel)) {
            lv_dropdown_set_selected(dd, (uint32_t)bsel);
        }
        c->llm_applied_backend = scr_backend_for_sel(
            lv_dropdown_get_selected(dd));
        lv_textarea_set_text(bu, c->fields.llm_base_url);
        lv_textarea_set_text(mo, c->fields.llm_model);
        lv_textarea_set_text(ak, c->fields.llm_api_key);
    }

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 84, 38);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -100, 2);
    lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_back);
    lv_obj_t *backl = lv_label_create(back);
    lv_label_set_text(backl, "Back");
    lv_obj_center(backl);

    lv_obj_t *next = lv_button_create(scr);
    lv_obj_set_size(next, 90, 38);
    lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -6, 2);
    lv_obj_add_event_cb(next, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_next);
    lv_obj_t *nl = lv_label_create(next);
    lv_label_set_text(nl, "Next");
    lv_obj_center(nl);

    lv_obj_t *err = lv_label_create(scr);
    lv_label_set_text(err, "");
    lv_obj_set_style_text_color(err, lv_color_make(0xFF, 0x60, 0x60),
                                0);
    lv_obj_set_width(err, LV_PCT(92));
    lv_obj_align(err, LV_ALIGN_TOP_LEFT, 6, 210);
    c->err_label = err;

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    /* Bind the keyboard to whichever textarea gains focus (touch). */
    lv_keyboard_set_textarea(kb, bu);
    lv_obj_set_size(kb, LV_PCT(100), LV_PCT(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    /* FLOATING exempts the keyboard from the screen's scroll position
     * so it stays sticky at the viewport's bottom edge while the form
     * scrolls behind it (scr_ta_focus_cb scrolls the form to bring the
     * focused textarea above the keyboard). Without this the keyboard
     * scrolls with the rest of the content. */
    lv_obj_add_flag(kb, LV_OBJ_FLAG_FLOATING);
    c->kb = kb;

    /* Bottom spacer extends the screen's content past the viewport so
     * the screen becomes scrollable. scr_ta_focus_cb does manual
     * scroll-to-y math against the keyboard's top edge so the focused
     * textarea (especially "Model" and "API key" which would otherwise
     * sit behind the keyboard) is brought into view. */
    lv_obj_t *bottom_spacer = lv_obj_create(scr);
    lv_obj_set_size(bottom_spacer, 1, 1);
    lv_obj_set_pos(bottom_spacer, 0, 380);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);

    /* Prefill from the initially-selected backend (index 0 = OPENAI). */
    scr_llm_apply_backend_defaults(c);
}

/* Focus/defocus wiring: show the keyboard and re-target it at the
 * focused textarea; hide it when editing ends. On focus, also scroll
 * the textarea above the keyboard so the user can see what they are
 * typing into (the LLM screen's lower textareas — model + api_key —
 * sit behind the 45% bottom keyboard area without scrolling).
 * Registered per-textarea by scr_llm_bind_kb(). */
static void scr_ta_focus_cb(lv_event_t *e)
{
    setup_screens_ctx_t *c =
        (setup_screens_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (c == NULL || c->kb == NULL || ta == NULL) {
        return;
    }
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(c->kb, ta);
        lv_obj_remove_flag(c->kb, LV_OBJ_FLAG_HIDDEN);
        /* Scroll the screen so the focused textarea's TOP sits at a
         * fixed safe y above the keyboard (independent of LVGL's not-
         * yet-laid-out textarea height at focus time). The widget's
         * label still sits ~18px above the textarea in the LLM screen
         * layout, so a top y of 22 keeps the label visible too. */
        const int32_t TA_TARGET_TOP_Y = 22;
        int32_t ta_y = lv_obj_get_y(ta);
        int32_t scroll_y = ta_y - TA_TARGET_TOP_Y;
        if (scroll_y < 0) {
            scroll_y = 0;
        }
        lv_obj_scroll_to_y(lv_screen_active(), scroll_y, LV_ANIM_ON);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(c->kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(lv_screen_active(), 0, LV_ANIM_ON);
    }
}

/* Intercept the keyboard's bottom-left mode-switcher button
 * (LV_SYMBOL_KEYBOARD): instead of cycling text/number/special modes,
 * dismiss the keyboard so the user can return to seeing the form.
 * This matches the "tap-keyboard-to-hide" idiom common to mobile UIs.
 * The LVGL default LV_EVENT_VALUE_CHANGED handler still runs first
 * and toggles the mode; our hide trumps that on this code path. */
static void scr_kb_value_changed_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target_obj(e);
    if (kb == NULL) {
        return;
    }
    uint32_t btn_id = lv_buttonmatrix_get_selected_button(kb);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(kb, btn_id);
    if (txt != NULL && strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_y(lv_screen_active(), 0, LV_ANIM_ON);
    }
}

static void scr_llm_bind_kb(setup_screens_ctx_t *c)
{
    lv_obj_t *tas[3] = {
        c->llm_base_url_ta, c->llm_model_ta, c->llm_api_key_ta
    };
    for (int i = 0; i < 3; i++) {
        if (tas[i] == NULL) {
            continue;
        }
        lv_obj_add_event_cb(tas[i], scr_ta_focus_cb, LV_EVENT_FOCUSED, c);
        lv_obj_add_event_cb(tas[i], scr_ta_focus_cb, LV_EVENT_DEFOCUSED,
                            c);
    }
    if (c->kb != NULL) {
        lv_obj_add_event_cb(c->kb, scr_kb_value_changed_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }
}

/* Mask a secret for the REVIEW summary: never show cleartext. Shows a
 * fixed bullet run if non-empty, "(none)" if empty. */
static const char *scr_mask_secret(const char *s)
{
    return (s != NULL && s[0] != '\0') ? "********" : "(none)";
}

static void scr_build_review(setup_screens_ctx_t *c)
{
    scr_clear_screen(c);
    scr_style_screen();
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Review");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 2);

    /* Summary: SSID, backend, base_url, model in clear; wifi password
     * and api key MASKED (never cleartext). */
    lv_obj_t *sum = lv_label_create(scr);
    lv_obj_set_style_text_color(sum, lv_color_make(0xD0, 0xD8, 0xE0), 0);
    lv_obj_set_width(sum, LV_PCT(92));
    lv_label_set_long_mode(sum, LV_LABEL_LONG_WRAP);
    lv_label_set_text_fmt(
        sum,
        "WiFi SSID: %s\n"
        "WiFi pass: %s\n"
        "Backend: %s\n"
        "Base URL: %s\n"
        "Model: %s\n"
        "API key: %s",
        c->fields.wifi_ssid,
        scr_mask_secret(c->fields.wifi_password),
        c->fields.llm_backend_type,
        c->fields.llm_base_url,
        c->fields.llm_model,
        scr_mask_secret(c->fields.llm_api_key));
    lv_obj_align(sum, LV_ALIGN_TOP_LEFT, 6, 26);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 100, 44);
    lv_obj_align(back, LV_ALIGN_BOTTOM_LEFT, 6, -6);
    lv_obj_add_event_cb(back, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_back);
    lv_obj_t *backl = lv_label_create(back);
    lv_label_set_text(backl, "Back");
    lv_obj_center(backl);

    lv_obj_t *save = lv_button_create(scr);
    lv_obj_set_size(save, 120, 44);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
    lv_obj_add_event_cb(save, scr_evt_set_bool, LV_EVENT_CLICKED,
                        &c->ev_save);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "Save");
    lv_obj_center(sl);

    lv_obj_t *err = lv_label_create(scr);
    lv_label_set_text(err, "");
    lv_obj_set_style_text_color(err, lv_color_make(0xFF, 0x60, 0x60),
                                0);
    lv_obj_set_width(err, LV_PCT(92));
    lv_obj_align(err, LV_ALIGN_BOTTOM_MID, 0, -56);
    c->err_label = err;
}

/* ---- helpers ---------------------------------------------------- */

/* Pump LVGL + drain worker results once. Always safe on LVGL task. */
static void scr_pump(void)
{
    setup_worker_poll_results();
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(SETUP_SCREENS_STEP_MS));
}

static void scr_clear_events(setup_screens_ctx_t *c)
{
    c->ev_start = false;
    c->ev_rescan = false;
    c->ev_manual = false;
    c->ev_pick = false;
    c->ev_pick_idx = -1;
    c->ev_connect = false;
    c->ev_back = false;
    c->ev_show_pw = false;
    c->ev_llm_backend = false;
    c->ev_next = false;
    c->ev_save = false;
    c->ev_menu_wifi = false;
    c->ev_menu_llm = false;
    c->ev_menu_status = false;
}

/* ---- public ----------------------------------------------------- */

setup_screens_ctx_t *setup_screens_ctx_create(setup_sm_mode_t mode)
{
    setup_screens_ctx_t *c = heap_caps_calloc(
        1, sizeof(*c), MALLOC_CAP_DEFAULT);
    if (c == NULL) {
        ESP_LOGE(TAG, "ctx alloc failed (%u B)",
                 (unsigned)sizeof(*c));
        return NULL;
    }
    c->mode = mode;
    /* FIRST_USE enters at WELCOME; SETTINGS has no WELCOME and enters
     * at the MENU hub. setup_screens_run() builds the matching screen. */
    c->state = (mode == SETUP_MODE_FIRST_USE) ? SETUP_ST_WELCOME
                                              : SETUP_ST_MENU;
    c->ev_pick_idx = -1;
    /* I1: invalid sentinel (calloc zero == SETUP_BACKEND_OPENAI, a
     * valid backend, so set explicitly) — no backend defaults applied
     * yet. */
    c->llm_applied_backend = SETUP_BACKEND_COUNT;
    return c;
}

void setup_screens_ctx_destroy(setup_screens_ctx_t *ctx)
{
    if (ctx != NULL) {
        free(ctx);
    }
}

esp_err_t setup_screens_run(void *ctx)
{
    setup_screens_ctx_t *c = (setup_screens_ctx_t *)ctx;
    if (c == NULL) {
        ESP_LOGE(TAG, "run: NULL ctx");
        return ESP_ERR_INVALID_ARG;
    }

    /* SETTINGS: seed c->fields from the model draft (the CURRENT loaded
     * config) ONCE, before any screen builds. setup_ui.c calls
     * setup_model_init() AFTER setup_screens_ctx_create() but BEFORE
     * setup_lvgl_port_run_screens(setup_screens_run, ...), so the draft is
     * populated by the time we get here. Without this the calloc-zeroed
     * c->fields would be persisted by the SAVE handler (it unconditionally
     * writes ALL fields back), wiping working WiFi creds on an LLM-only
     * settings save, and the reused WiFi/LLM screens would show blanks
     * instead of the current config. Mirror EXACTLY the field set the SAVE
     * handler writes back so untouched fields round-trip identically.
     * No-brick: a NULL draft logs and proceeds (screens just show blank,
     * same as before) — never abort. FIRST_USE is untouched: this seed
     * is gated to SETTINGS mode only. */
    if (c->mode == SETUP_MODE_SETTINGS) {
        app_config_t *d = (app_config_t *)setup_model_draft();
        if (d == NULL) {
            ESP_LOGW(TAG,
                     "SETTINGS: draft NULL (init missed) — fields blank");
        } else {
            strlcpy(c->fields.wifi_ssid, d->wifi_ssid,
                    sizeof(c->fields.wifi_ssid));
            strlcpy(c->fields.wifi_password, d->wifi_password,
                    sizeof(c->fields.wifi_password));
            strlcpy(c->fields.llm_backend_type, d->llm_backend_type,
                    sizeof(c->fields.llm_backend_type));
            strlcpy(c->fields.llm_base_url, d->llm_base_url,
                    sizeof(c->fields.llm_base_url));
            strlcpy(c->fields.llm_model, d->llm_model,
                    sizeof(c->fields.llm_model));
            strlcpy(c->fields.llm_api_key, d->llm_api_key,
                    sizeof(c->fields.llm_api_key));
        }
    }

    setup_worker_set_result_cb(scr_result_cb, c);

    /* Entry screen is mode-specific (set by setup_screens_ctx_create):
     * FIRST_USE starts at WELCOME, SETTINGS starts at the MENU hub. The
     * SM gates every onward edge from there. */
    scr_clear_events(c);
    if (c->state == SETUP_ST_MENU) {
        scr_build_menu(c);
    } else {
        c->state = SETUP_ST_WELCOME;
        scr_build_welcome(c);
    }

    bool scan_pending = false;   /* a scan job is in flight */
    bool connect_pending = false;
    bool need_scan_kick = false; /* request a scan on entering SCAN */

    while (true) {
        /* Cooperative stop: the port owner asked the LVGL task to tear
         * down. This loop must NOT be able to run forever with no exit
         * other than WiFi success — otherwise a stuck flow pins the
         * owner task (and app_main). Observe the stop promptly, clean
         * up via the shared done: path, and return non-fatal so the
         * caller's no-brick unwind restores emote. */
        if (setup_lvgl_port_should_stop()) {
            ESP_LOGW(TAG, "stop requested — ending screen flow");
            goto done;
        }

        scr_pump();

        switch (c->state) {

        case SETUP_ST_WELCOME:
            if (c->ev_start) {
                c->ev_start = false;
                c->state = setup_sm_next(c->state, SETUP_EV_START,
                                         c->mode);
                if (c->state == SETUP_ST_WIFI_SCAN) {
                    need_scan_kick = true;
                    scr_build_wifi_scan(c, true);
                }
            }
            break;

        case SETUP_ST_WIFI_SCAN:
            if (need_scan_kick && !scan_pending) {
                need_scan_kick = false;
                c->job_done = false;
                c->job_running = true;
                scan_pending = true;
                esp_err_t s = setup_worker_submit(scr_scan_job, c);
                if (s != ESP_OK) {
                    /* Could not even queue the scan: fail soft —
                     * show an empty list so the user can Rescan /
                     * Manual SSID. Never brick. */
                    ESP_LOGE(TAG, "scan submit failed: %s",
                             esp_err_to_name(s));
                    scan_pending = false;
                    c->job_running = false;
                    c->ap_count = 0;
                    scr_build_wifi_scan(c, false);
                }
            }
            if (scan_pending && c->job_done &&
                c->job_kind == SCR_JOB_SCAN) {
                scan_pending = false;
                c->job_running = false;
                if (c->job_status != ESP_OK) {
                    ESP_LOGW(TAG, "scan failed: %s (empty list)",
                             esp_err_to_name(c->job_status));
                    c->ap_count = 0;
                }
                scr_build_wifi_scan(c, false);
                break;
            }
            if (c->ev_back) {
                c->ev_back = false;
                /* SETTINGS-only escape: the Back button is built only in
                 * SETTINGS mode, and the SM only has WIFI_SCAN+BACK for
                 * !first_use, so FIRST_USE can never reach here. Guard the
                 * rebuild by the RESULTING state (not the mode), consistent
                 * with the other return-to-MENU sites. */
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                if (c->state == SETUP_ST_MENU) {
                    /* Abandon any in-flight / completed scan so a later
                     * WiFi re-entry from MENU re-kicks a FRESH scan and
                     * never consumes a stale c->job_done or shows the
                     * previous scan's c->aps[]. Mirror the consumed-scan
                     * bookkeeping used by the scan-kick branch
                     * (need_scan_kick/scan_pending/c->job_done): a worker
                     * job may still be in flight, but the ctx outlives
                     * the worker, so a late scr_result_cb writing
                     * aps[]/job_done is harmless — the re-entry path
                     * clears c->job_done again and re-kicks before any
                     * stale completion can be consumed. No abort. */
                    need_scan_kick = false;
                    scan_pending = false;
                    c->job_done = false;
                    scr_build_menu(c);
                }
                break;
            }
            if (c->ev_rescan && !scan_pending) {
                c->ev_rescan = false;
                need_scan_kick = true;
                scr_build_wifi_scan(c, true);
            }
            if (c->ev_manual && !scan_pending) {
                c->ev_manual = false;
                scr_build_manual_ssid(c);
                /* Reuse ev_pick to mean "manual SSID confirmed". */
                c->ev_pick = false;
                c->ev_pick_idx = -1;
            }
            if (c->ev_pick) {
                c->ev_pick = false;
                if (c->ssid_ta != NULL) {
                    /* Manual SSID path. */
                    const char *txt =
                        lv_textarea_get_text(c->ssid_ta);
                    if (txt == NULL || txt[0] == '\0') {
                        ESP_LOGW(TAG, "manual SSID empty — ignored");
                        break;
                    }
                    strlcpy(c->fields.wifi_ssid, txt,
                            sizeof(c->fields.wifi_ssid));
                } else if (c->ev_pick_idx >= 0 &&
                           c->ev_pick_idx < (int)c->ap_count) {
                    strlcpy(c->fields.wifi_ssid,
                            c->aps[c->ev_pick_idx].ssid,
                            sizeof(c->fields.wifi_ssid));
                } else {
                    ESP_LOGW(TAG, "pick with bad idx %d — ignored",
                             c->ev_pick_idx);
                    break;
                }
                c->fields.wifi_password[0] = '\0';
                c->state = setup_sm_next(c->state,
                                         SETUP_EV_PICK_WIFI,
                                         c->mode);
                if (c->state == SETUP_ST_WIFI_PW) {
                    scr_build_wifi_pw(c);
                }
            }
            break;

        case SETUP_ST_WIFI_PW:
            if (c->ev_show_pw) {
                c->ev_show_pw = false;
                if (c->pw_ta != NULL) {
                    c->pw_hidden = !c->pw_hidden;
                    lv_textarea_set_password_mode(c->pw_ta,
                                                  c->pw_hidden);
                }
            }
            if (c->ev_back && !connect_pending) {
                c->ev_back = false;
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                if (c->state == SETUP_ST_WIFI_SCAN) {
                    /* Re-show cached APs immediately (no re-scan). */
                    scr_build_wifi_scan(c, false);
                }
                break;
            }
            if (c->ev_connect && !connect_pending) {
                c->ev_connect = false;
                if (c->pw_ta != NULL) {
                    const char *pw =
                        lv_textarea_get_text(c->pw_ta);
                    strlcpy(c->fields.wifi_password,
                            (pw != NULL) ? pw : "",
                            sizeof(c->fields.wifi_password));
                }
                const char *verr = NULL;
                if (!setup_validate_wifi(&c->fields, &verr)) {
                    if (c->err_label != NULL) {
                        lv_label_set_text(
                            c->err_label,
                            (verr != NULL) ? verr
                                           : "Invalid WiFi input");
                    }
                    ESP_LOGW(TAG, "validate_wifi failed: %s",
                             (verr != NULL) ? verr : "?");
                    break;
                }
                if (c->err_label != NULL) {
                    lv_label_set_text(c->err_label, "Connecting...");
                    lv_obj_set_style_text_color(
                        c->err_label,
                        lv_color_make(0xC0, 0xC8, 0xD0), 0);
                }
                c->job_done = false;
                c->job_running = true;
                connect_pending = true;
                esp_err_t s =
                    setup_worker_submit(scr_connect_job, c);
                if (s != ESP_OK) {
                    connect_pending = false;
                    c->job_running = false;
                    if (c->err_label != NULL) {
                        lv_label_set_text(c->err_label,
                                          "Busy — try again");
                        lv_obj_set_style_text_color(
                            c->err_label,
                            lv_color_make(0xFF, 0x60, 0x60), 0);
                    }
                    ESP_LOGE(TAG, "connect submit failed: %s",
                             esp_err_to_name(s));
                }
            }
            if (connect_pending && c->job_done &&
                c->job_kind == SCR_JOB_CONNECT) {
                connect_pending = false;
                c->job_running = false;
                if (c->job_status == ESP_OK) {
                    wifi_manager_status_t st;
                    memset(&st, 0, sizeof(st));
                    wifi_manager_get_status(&st);
                    ESP_LOGI(TAG,
                             "WiFi connected, STA IP=%s",
                             (st.sta_ip != NULL) ? st.sta_ip
                                                 : "(none)");
                    c->state = setup_sm_next(c->state,
                                             SETUP_EV_CONNECT_OK,
                                             c->mode);
                    /* Guard by the RESULTING state, not the mode: the SM
                     * routes CONNECT_OK to LLM (FIRST_USE) or back to the
                     * MENU hub (SETTINGS — WiFi is already live-applied by
                     * scr_connect_job's wifi_manager_apply_sta_config, so
                     * the WiFi subflow is done). */
                    if (c->state == SETUP_ST_LLM) {
                        scr_build_llm(c);
                        scr_llm_bind_kb(c);
                    } else if (c->state == SETUP_ST_MENU) {
                        scr_build_menu(c);
                    } else {
                        ESP_LOGW(TAG,
                                 "connect-OK but state=%d (not LLM/MENU) "
                                 "— ending flow (no brick)",
                                 (int)c->state);
                        goto done;
                    }
                    break;
                }
                ESP_LOGW(TAG, "connect failed: %s (stay on PW)",
                         esp_err_to_name(c->job_status));
                c->state = setup_sm_next(c->state,
                                         SETUP_EV_CONNECT_FAIL,
                                         c->mode);
                if (c->err_label != NULL) {
                    lv_label_set_text(
                        c->err_label,
                        "Connection failed — check password");
                    lv_obj_set_style_text_color(
                        c->err_label,
                        lv_color_make(0xFF, 0x60, 0x60), 0);
                }
            }
            break;

        case SETUP_ST_LLM:
            /* Backend dropdown changed -> re-prefill base_url/model from
             * setup_backend_defaults() (textareas stay user-editable). */
            if (c->ev_llm_backend) {
                c->ev_llm_backend = false;
                scr_llm_apply_backend_defaults(c);
            }
            if (c->ev_back) {
                c->ev_back = false;
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                /* Guard by the RESULTING state, not the mode: SM routes
                 * LLM+BACK to WIFI_PW (FIRST_USE) or the MENU hub
                 * (SETTINGS). */
                if (c->state == SETUP_ST_WIFI_PW) {
                    /* Back to the WiFi password screen (SM first-use
                     * edge). Re-show it; the SSID is still in
                     * ctx->fields from 4.2. */
                    scr_build_wifi_pw(c);
                } else if (c->state == SETUP_ST_MENU) {
                    scr_build_menu(c);
                } else {
                    ESP_LOGW(TAG,
                             "LLM Back but state=%d (not WIFI_PW/MENU) — "
                             "ending flow (no brick)",
                             (int)c->state);
                    goto done;
                }
                break;
            }
            if (c->ev_next) {
                c->ev_next = false;
                /* Copy LLM widget values into ctx->fields, keeping the
                 * wifi_ssid/wifi_password typed back in 4.2. */
                if (c->llm_backend_dd != NULL) {
                    setup_backend_t b = scr_backend_for_sel(
                        lv_dropdown_get_selected(c->llm_backend_dd));
                    strlcpy(c->fields.llm_backend_type,
                            setup_backend_id(b),
                            sizeof(c->fields.llm_backend_type));
                }
                if (c->llm_base_url_ta != NULL) {
                    const char *t =
                        lv_textarea_get_text(c->llm_base_url_ta);
                    strlcpy(c->fields.llm_base_url,
                            (t != NULL) ? t : "",
                            sizeof(c->fields.llm_base_url));
                }
                if (c->llm_model_ta != NULL) {
                    const char *t =
                        lv_textarea_get_text(c->llm_model_ta);
                    strlcpy(c->fields.llm_model,
                            (t != NULL) ? t : "",
                            sizeof(c->fields.llm_model));
                }
                if (c->llm_api_key_ta != NULL) {
                    const char *t =
                        lv_textarea_get_text(c->llm_api_key_ta);
                    strlcpy(c->fields.llm_api_key,
                            (t != NULL) ? t : "",
                            sizeof(c->fields.llm_api_key));
                }
                const char *verr = NULL;
                if (!setup_validate_llm(&c->fields, &verr)) {
                    if (c->err_label != NULL) {
                        lv_label_set_text(
                            c->err_label,
                            (verr != NULL) ? verr
                                           : "Invalid LLM input");
                    }
                    ESP_LOGW(TAG, "validate_llm failed: %s",
                             (verr != NULL) ? verr : "?");
                    break;
                }
                c->state = setup_sm_next(c->state, SETUP_EV_NEXT,
                                         c->mode);
                if (c->state == SETUP_ST_REVIEW) {
                    scr_build_review(c);
                } else {
                    ESP_LOGW(TAG,
                             "LLM Next but state=%d (not REVIEW) — "
                             "ending flow (no brick)",
                             (int)c->state);
                    goto done;
                }
            }
            break;

        case SETUP_ST_REVIEW:
            if (c->ev_back) {
                c->ev_back = false;
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                if (c->state == SETUP_ST_LLM) {
                    scr_build_llm(c);
                    scr_llm_bind_kb(c);
                    /* Re-seat the dropdown + textareas from the values
                     * the user already entered (kept in ctx->fields). */
                    if (c->llm_backend_dd != NULL) {
                        setup_backend_t bsel = SETUP_BACKEND_CUSTOM;
                        if (setup_backend_from_id(
                                c->fields.llm_backend_type, &bsel)) {
                            lv_dropdown_set_selected(
                                c->llm_backend_dd, (uint32_t)bsel);
                        }
                    }
                    if (c->llm_base_url_ta != NULL) {
                        lv_textarea_set_text(c->llm_base_url_ta,
                                             c->fields.llm_base_url);
                    }
                    if (c->llm_model_ta != NULL) {
                        lv_textarea_set_text(c->llm_model_ta,
                                             c->fields.llm_model);
                    }
                    if (c->llm_api_key_ta != NULL) {
                        lv_textarea_set_text(c->llm_api_key_ta,
                                             c->fields.llm_api_key);
                    }
                    /* I1: the lv_dropdown_set_selected() above synthesizes
                     * a VALUE_CHANGED that set c->ev_llm_backend; clear it
                     * so the next loop iteration does NOT run
                     * scr_llm_apply_backend_defaults() and overwrite the
                     * base_url/model we just restored from c->fields.
                     * Also align llm_applied_backend with the restored
                     * selection so a later real user re-pick of a
                     * different backend still prefills as per spec. */
                    c->ev_llm_backend = false;
                    if (c->llm_backend_dd != NULL) {
                        c->llm_applied_backend = scr_backend_for_sel(
                            lv_dropdown_get_selected(c->llm_backend_dd));
                    }
                } else {
                    ESP_LOGW(TAG,
                             "REVIEW Back but state=%d (not LLM) — "
                             "ending flow (no brick)",
                             (int)c->state);
                    goto done;
                }
                break;
            }
            if (c->ev_save) {
                c->ev_save = false;
                c->state = setup_sm_next(c->state, SETUP_EV_SAVE,
                                         c->mode);
                if (c->state != SETUP_ST_SAVE) {
                    ESP_LOGW(TAG,
                             "REVIEW Save but state=%d (not SAVE) — "
                             "ending flow (no brick)",
                             (int)c->state);
                    goto done;
                }
                /* fall into SAVE handling on the next loop iteration */
            }
            break;

        case SETUP_ST_SAVE: {
            /* Populate the model draft from the validated fields, then
             * commit. setup_model_draft() concrete type is
             * app_config_t* (this is an ESP TU). Every failure path is
             * no-brick: inline error + stay, never esp_restart on a
             * real save failure. */
            app_config_t *d = (app_config_t *)setup_model_draft();
            if (d == NULL) {
                ESP_LOGE(TAG, "SAVE: draft NULL (init missed)");
                if (c->err_label != NULL) {
                    lv_label_set_text(c->err_label,
                                      "Internal error — not saved");
                }
                /* Stay on REVIEW so the user can retry/Back. */
                c->state = SETUP_ST_REVIEW;
                break;
            }
            strlcpy(d->wifi_ssid, c->fields.wifi_ssid,
                    sizeof(d->wifi_ssid));
            strlcpy(d->wifi_password, c->fields.wifi_password,
                    sizeof(d->wifi_password));
            strlcpy(d->llm_backend_type, c->fields.llm_backend_type,
                    sizeof(d->llm_backend_type));
            strlcpy(d->llm_base_url, c->fields.llm_base_url,
                    sizeof(d->llm_base_url));
            strlcpy(d->llm_model, c->fields.llm_model,
                    sizeof(d->llm_model));
            strlcpy(d->llm_api_key, c->fields.llm_api_key,
                    sizeof(d->llm_api_key));

            int rc = setup_model_commit();
            /* Tri-state:
             *   ESP_OK               -> persisted + live-refreshed.
             *   ESP_ERR_INVALID_STATE-> persisted to NVS, in-memory NOT
             *                           refreshed. NON-FATAL for
             *                           FIRST_USE (we reboot, which
             *                           reloads config fresh).
             *   anything else        -> real app_config_save() failure:
             *                           inline error, STAY, do NOT
             *                           restart (let the user retry). */
            if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
                ESP_LOGI(TAG, "setup_model_commit rc=%s (saved)",
                         esp_err_to_name((esp_err_t)rc));
                if (c->mode == SETUP_MODE_FIRST_USE) {
                    scr_build_interim(c, "Saved — restarting");
                    int steps = SETUP_SCREENS_INTERIM_MS /
                                SETUP_SCREENS_STEP_MS;
                    for (int i = 0; i < steps; i++) {
                        scr_pump();
                    }
                    ESP_LOGI(TAG,
                             "first-use save complete — esp_restart()");
                    esp_restart();
                    /* not reached */
                }
                /* SETTINGS N4 policy: do NOT esp_restart. WiFi was
                 * already live-applied by scr_connect_job's
                 * wifi_manager_apply_sta_config on CONNECT_OK (both
                 * modes) — do not apply again. llm_* only takes effect
                 * via app_claw at boot, hence the "reboot required for
                 * LLM" wording. Show a brief notice, then drive the SM
                 * SAVE -> DONE so the state becomes MENU and rebuild the
                 * hub. */
                ESP_LOGI(TAG,
                         "SETTINGS save complete — notice + return to "
                         "MENU (no restart)");
                scr_build_interim(
                    c,
                    "Saved. Wi-Fi applied; reboot required for LLM "
                    "changes.");
                int steps = SETUP_SCREENS_INTERIM_MS /
                            SETUP_SCREENS_STEP_MS;
                for (int i = 0; i < steps; i++) {
                    scr_pump();
                }
                c->state = setup_sm_next(c->state, SETUP_EV_DONE,
                                         c->mode);
                if (c->state == SETUP_ST_MENU) {
                    scr_build_menu(c);
                } else {
                    ESP_LOGW(TAG,
                             "SAVE done but state=%d (not MENU) — "
                             "ending flow (no brick)",
                             (int)c->state);
                    goto done;
                }
                break;
            }
            ESP_LOGE(TAG, "setup_model_commit failed: %s (stay)",
                     esp_err_to_name((esp_err_t)rc));
            if (c->err_label != NULL) {
                lv_label_set_text(c->err_label,
                                  "Save failed — try again");
                lv_obj_set_style_text_color(
                    c->err_label,
                    lv_color_make(0xFF, 0x60, 0x60), 0);
            }
            /* Stay on REVIEW so the user can retry Save or go Back. */
            c->state = SETUP_ST_REVIEW;
            break;
        }

        case SETUP_ST_MENU:
            /* SETTINGS hub. Map each MENU intent through the pure SM and
             * build the target subflow screen on the resulting state.
             * Clear the MENU intents after handling (mirrors the
             * scr_clear_events idiom for one-shot intents). */
            if (c->ev_menu_wifi) {
                c->ev_menu_wifi = false;
                c->state = setup_sm_next(c->state, SETUP_EV_PICK_WIFI,
                                         c->mode);
                if (c->state == SETUP_ST_WIFI_SCAN) {
                    /* Same WIFI_SCAN entry as WELCOME->WIFI_SCAN: show
                     * the scanning screen and kick a scan. */
                    need_scan_kick = true;
                    scr_build_wifi_scan(c, true);
                }
                break;
            }
            if (c->ev_menu_llm) {
                c->ev_menu_llm = false;
                c->state = setup_sm_next(c->state, SETUP_EV_PICK_LLM,
                                         c->mode);
                if (c->state == SETUP_ST_LLM) {
                    scr_build_llm(c);
                    scr_llm_bind_kb(c);
                }
                break;
            }
            if (c->ev_menu_status) {
                c->ev_menu_status = false;
                c->state = setup_sm_next(c->state, SETUP_EV_PICK_STATUS,
                                         c->mode);
                if (c->state == SETUP_ST_STATUS) {
                    scr_build_status(c);
                    scr_status_refresh(c);                    /* first paint */
                    c->status_last_refresh_us = esp_timer_get_time();
                }
                break;
            }
            if (c->ev_back) {
                c->ev_back = false;
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                /* SM routes MENU+BACK -> EXIT; the next iteration's
                 * SETUP_ST_EXIT case ends the flow via the shared
                 * done: path so the caller's reverse-order unwind
                 * resumes emote. */
            }
            break;

        case SETUP_ST_STATUS:
            /* Read-only Settings -> STATUS. 0.2 Hz live refresh against
             * the snapshot APIs (wifi_manager_get_status,
             * claw_core_llm_get_{last_call,config_summary}); the labels
             * are stowed in c->status_lbl_* by scr_build_status() so
             * refresh re-texts in place without rebuilding the screen.
             * Back returns to the MENU hub (same return-to-MENU pattern
             * as WIFI_SCAN / LLM). */
            if (esp_timer_get_time() - c->status_last_refresh_us
                    >= 5LL * 1000LL * 1000LL) {        /* 0.2 Hz tick */
                scr_status_refresh(c);
                c->status_last_refresh_us = esp_timer_get_time();
            }
            if (c->ev_back) {
                c->ev_back = false;
                c->state = setup_sm_next(c->state, SETUP_EV_BACK,
                                         c->mode);
                if (c->state == SETUP_ST_MENU) {
                    scr_build_menu(c);
                }
            }
            break;

        case SETUP_ST_EXIT:
            /* Terminal (settings done / MENU Back). Leave the loop
             * through the shared clean-up path and return ESP_OK so
             * setup_ui.c performs its reverse-order unwind (release
             * arbiter -> emote resumes). Never esp_restart / abort. */
            ESP_LOGI(TAG, "EXIT — ending settings flow");
            goto done;

        default:
            /* No other states are built in Task 4.2/4.3; reaching one
             * is a logic slip — log and stop cleanly (no brick). */
            ESP_LOGW(TAG, "unexpected state %d — ending flow",
                     (int)c->state);
            goto done;
        }
    }

done:
    /* Leak-gate discipline: destroy every object we created while the
     * display is still valid and we are on the LVGL task, mirroring
     * the built-in pattern teardown in setup_lvgl_port.c. */
    setup_worker_set_result_cb(NULL, NULL);
    scr_clear_screen(c);
    lv_obj_clean(lv_screen_active());
    ESP_LOGI(TAG, "setup_screens_run -> ESP_OK");
    return ESP_OK;
}
