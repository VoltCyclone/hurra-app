/*
 * tf_config.h — TinyFrame configuration for the Hurra host adapter.
 *
 * Must stay in sync with imxrtnsy/src/TF_Config.h. Discrepancies between the
 * two ends will produce silent framing failures.  See spec §2.
 */
#pragma once

#define TF_SOF_BYTE       0x68      /* 'h' for Hurra */
#define TF_ID_BYTES       1
#define TF_LEN_BYTES      1
#define TF_TYPE_BYTES     1
#define TF_CKSUM_TYPE     TF_CKSUM_CRC16
#define TF_USE_MUTEX      0
#define TF_MAX_PAYLOAD_RX 256
#define TF_SENDBUF_LEN    264

/* Host calls TF_Tick approximately every 1 ms inside hurra_poll(); 50 ticks
 * = 50 ms parser timeout. Generous compared with the firmware's 5-tick (5 ms)
 * value, but the host has no real-time constraint.
 */
#define TF_PARSER_TIMEOUT_TICKS 50

#include <stdint.h>
typedef uint32_t TF_TICKS;
typedef uint8_t  TF_COUNT;

#define TF_MAX_ID_LST    8     /* in-flight requests; one per outstanding reply */
#define TF_MAX_TYPE_LST  16    /* host subscribes to TLM_* only */
#define TF_MAX_GEN_LST   1     /* TinyFrame requires ≥1 */

#define TF_Error(...) ((void)0)
