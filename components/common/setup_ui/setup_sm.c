#include <stdbool.h>
#include "setup_sm.h"

/* Pure transition table. Any (state,event,mode) not explicitly handled falls
 * through to "return state" (stay). No I/O, no globals, no field logic. */

setup_sm_state_t setup_sm_next(setup_sm_state_t state,
                               setup_sm_event_t event,
                               setup_sm_mode_t mode)
{
    bool first_use = (mode == SETUP_MODE_FIRST_USE);

    switch (state) {
    case SETUP_ST_WELCOME:
        /* WELCOME exists only in the first-use flow. */
        if (first_use && event == SETUP_EV_START) {
            return SETUP_ST_WIFI_SCAN;
        }
        break;

    case SETUP_ST_WIFI_SCAN:
        if (event == SETUP_EV_PICK_WIFI) {
            return SETUP_ST_WIFI_PW;
        }
        if (!first_use && event == SETUP_EV_BACK) {
            return SETUP_ST_MENU;
        }
        break;

    case SETUP_ST_WIFI_PW:
        if (event == SETUP_EV_CONNECT_OK) {
            /* first-use: continue to LLM; settings: WiFi subflow done -> MENU */
            return first_use ? SETUP_ST_LLM : SETUP_ST_MENU;
        }
        if (event == SETUP_EV_CONNECT_FAIL) {
            return SETUP_ST_WIFI_PW; /* stay */
        }
        if (event == SETUP_EV_BACK) {
            return SETUP_ST_WIFI_SCAN;
        }
        break;

    case SETUP_ST_LLM:
        if (event == SETUP_EV_NEXT) {
            return SETUP_ST_REVIEW;
        }
        if (event == SETUP_EV_BACK) {
            /* first-use: back to WiFi password; settings: back to MENU */
            return first_use ? SETUP_ST_WIFI_PW : SETUP_ST_MENU;
        }
        break;

    case SETUP_ST_REVIEW:
        if (event == SETUP_EV_SAVE) {
            return SETUP_ST_SAVE;
        }
        if (event == SETUP_EV_BACK) {
            return SETUP_ST_LLM;
        }
        break;

    case SETUP_ST_SAVE:
        if (event == SETUP_EV_DONE) {
            /* first-use: setup complete -> EXIT; settings: return to MENU */
            return first_use ? SETUP_ST_EXIT : SETUP_ST_MENU;
        }
        break;

    case SETUP_ST_MENU:
        /* MENU is the settings hub; no MENU in the first-use flow. */
        if (!first_use) {
            if (event == SETUP_EV_PICK_WIFI) {
                return SETUP_ST_WIFI_SCAN;
            }
            if (event == SETUP_EV_PICK_LLM) {
                return SETUP_ST_LLM;
            }
            if (event == SETUP_EV_PICK_STATUS) {
                return SETUP_ST_STATUS;
            }
            if (event == SETUP_EV_BACK) {
                return SETUP_ST_EXIT;
            }
        }
        break;

    case SETUP_ST_STATUS:
        /* STATUS is a read-only leaf reachable only from MENU in the
         * settings flow; BACK returns to MENU. Every other event leaves
         * STATUS unchanged via the default stay-fallthrough below. */
        if (!first_use && event == SETUP_EV_BACK) {
            return SETUP_ST_MENU;
        }
        break;

    case SETUP_ST_EXIT:
        /* Terminal: no outgoing edges. */
        break;
    }

    return state; /* unknown / undefined edge: stay */
}
