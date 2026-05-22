#pragma once

/* Pure setup-UI navigation state machine. No I/O, no globals, deterministic.
 * setup_sm_next() is a referentially-transparent transition function:
 * an (state,event,mode) triple with no defined edge returns the SAME state
 * unchanged (unknown events are ignored, never crash).
 * SETUP_ST_EXIT is terminal (no outgoing edges); a driver reaching it MUST
 * tear down the setup session and release the display.
 *
 * This module intentionally holds NO config-field logic. Field/required
 * decisions live in setup_validate.c (setup_first_use_required / validators);
 * the SM is a pure transition table only and never re-derives them. */

typedef enum {
    SETUP_ST_WELCOME,
    SETUP_ST_WIFI_SCAN,
    SETUP_ST_WIFI_PW,
    SETUP_ST_LLM,
    SETUP_ST_REVIEW,
    SETUP_ST_SAVE,
    SETUP_ST_MENU,
    SETUP_ST_STATUS,
    SETUP_ST_EXIT
} setup_sm_state_t;

typedef enum {
    SETUP_MODE_FIRST_USE,
    SETUP_MODE_SETTINGS
} setup_sm_mode_t;

typedef enum {
    SETUP_EV_START,
    SETUP_EV_CONNECT_OK,
    SETUP_EV_CONNECT_FAIL,
    SETUP_EV_NEXT,
    SETUP_EV_SAVE,
    SETUP_EV_PICK_WIFI,
    SETUP_EV_PICK_LLM,
    SETUP_EV_PICK_STATUS,
    SETUP_EV_DONE,
    SETUP_EV_BACK
} setup_sm_event_t;

setup_sm_state_t setup_sm_next(setup_sm_state_t state,
                               setup_sm_event_t event,
                               setup_sm_mode_t mode);
