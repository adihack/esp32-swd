/* SPDX-License-Identifier: MIT */
/* esp-swd: per-channel isolation state machine and interlock (hardware-agnostic).
 *
 * Switch groups: VTREF (sense), SWD, UART. A signal group is closed only while it has a holder and the DUT is
 * powered (VTref inside the window after the settle time). The VTREF switch closes first and opens last.
 * Default: isolated. Host loss releases host-owned holders after a grace period.
 *
 * Drive it with esw_channel_tick() every few milliseconds; every switch change happens inside tick().
 */
#ifndef ESW_CHANNEL_H
#define ESW_CHANNEL_H
#include <stdint.h>
#include "esw/err.h"

typedef enum { ESW_GROUP_VTREF = 0, ESW_GROUP_SWD = 1, ESW_GROUP_UART = 2 } esw_group_t;
#define ESW_GROUP_COUNT 3u

/* Holders: who keeps a signal group closed. */
#define ESW_HOLD_HOST_JOB   (1u << 0)   /* host-requested SWD job or session */
#define ESW_HOLD_DAP        (1u << 1)   /* CMSIS-DAP DAP_Connect .. DAP_Disconnect */
#define ESW_HOLD_HOST_UART  (1u << 2)   /* UART stream subscribed in the vendor protocol */
#define ESW_HOLD_CDC_UART   (1u << 3)   /* CDC-ACM port open (DTR) */
#define ESW_HOLD_LOCAL      (1u << 8)   /* firmware-internal job; not released by host loss */
#define ESW_HOLD_HOST_MASK  (ESW_HOLD_HOST_JOB | ESW_HOLD_DAP | ESW_HOLD_HOST_UART | ESW_HOLD_CDC_UART)

typedef enum { ESW_GSTATE_OPEN = 0, ESW_GSTATE_WAIT_VTREF, ESW_GSTATE_CLOSED } esw_gstate_t;

typedef enum {
    ESW_EV_GROUP_CLOSED,
    ESW_EV_GROUP_OPENED,
    ESW_EV_VTREF_OK,
    ESW_EV_VTREF_BAD,
    ESW_EV_HOST_LOST,
} esw_ev_kind_t;

typedef enum { ESW_WHY_HOLDER, ESW_WHY_VTREF, ESW_WHY_HOST_LOSS, ESW_WHY_FORCED } esw_ev_why_t;

typedef struct {
    esw_ev_kind_t kind;
    esw_group_t group;
    esw_ev_why_t why;
    int32_t vtref_mv;
} esw_channel_event_t;

typedef struct {
    void (*set_switch)(void *ctx, esw_group_t g, uint32_t closed);
    int32_t (*read_vtref_mv)(void *ctx);                 /* DUT VTref in mV; called only while VTREF is closed */
    void (*prepare_lines)(void *ctx, esw_group_t g);      /* tool-side safe state right before g closes */
    void (*event)(void *ctx, const esw_channel_event_t *ev);   /* optional */
} esw_channel_ops_t;

typedef struct {
    int32_t vtref_min_mv;          /* r5: 2800 */
    int32_t vtref_max_mv;          /* r5: 3600 */
    uint32_t vtref_settle_ms;      /* r5: 50 (divider tau 5 ms) */
    uint32_t host_loss_grace_ms;   /* e.g. 2000 */
} esw_channel_cfg_t;

typedef struct {
    const esw_channel_ops_t *ops;
    void *ctx;
    esw_channel_cfg_t cfg;
    uint32_t holders[ESW_GROUP_COUNT];     /* [ESW_GROUP_VTREF] unused: VTREF follows the signal groups */
    esw_gstate_t gstate[ESW_GROUP_COUNT];
    uint8_t vtref_closed, vtref_valid, vtref_ok, host_present, host_loss_done;
    uint32_t vtref_since_ms, host_lost_ms;
    int32_t vtref_mv;
    uint32_t opened[ESW_GROUP_COUNT];      /* incremented every time a signal group opens (any reason): a session
                                              that saw the group closed at attach is stale once this changes */
} esw_channel_t;

/* Opens every switch immediately. */
void esw_channel_init(esw_channel_t *c, const esw_channel_ops_t *ops, void *ctx, const esw_channel_cfg_t *cfg);
esw_err_t esw_channel_hold(esw_channel_t *c, esw_group_t g, uint32_t holder);
esw_err_t esw_channel_release(esw_channel_t *c, esw_group_t g, uint32_t holder);
void esw_channel_set_host_present(esw_channel_t *c, uint32_t present, uint32_t now_ms);
void esw_channel_tick(esw_channel_t *c, uint32_t now_ms);
void esw_channel_isolate(esw_channel_t *c);   /* drop every holder and open everything now */
esw_gstate_t esw_channel_state(const esw_channel_t *c, esw_group_t g);

#endif
