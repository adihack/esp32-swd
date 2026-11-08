/* SPDX-License-Identifier: MIT */
#include "esw/err.h"

const char *esw_err_str(esw_err_t e)
{
    switch (e) {
    case ESW_OK: return "ok";
    case ESW_ERR_ARG: return "invalid argument";
    case ESW_ERR_NO_ACK: return "no SWD response";
    case ESW_ERR_WAIT: return "WAIT retries exhausted";
    case ESW_ERR_FAULT: return "FAULT / sticky error";
    case ESW_ERR_PARITY: return "read parity error";
    case ESW_ERR_PROTOCOL: return "invalid ACK";
    case ESW_ERR_TIMEOUT: return "timeout";
    case ESW_ERR_LOCKED: return "debug access locked";
    case ESW_ERR_VERIFY: return "verify mismatch";
    case ESW_ERR_STATE: return "invalid state";
    case ESW_ERR_UNSUPPORTED: return "unsupported";
    case ESW_ERR_NOT_FOUND: return "not found";
    case ESW_ERR_LOADER: return "loader error";
    case ESW_ERR_INTERLOCK: return "isolation interlock";
    }
    return "unknown error";
}
