/**
 * telemetry_proto.h - Shared wire format for the CAN -> LoRa -> PC telemetry chain.
 *
 * This file is the single source of truth for the protocol. It is compiled into
 * BOTH firmware images (Bluepill nodes and the L476RG hub) and is mirrored
 * byte-for-byte by pc_app/telemetry/proto.py.
 *
 * If you change anything here, change proto.py to match and re-run
 * tools/test_roundtrip.py.
 *
 * All multi-byte fields are LITTLE-ENDIAN (native for Cortex-M and x86, so no
 * byte swapping is needed on either end).
 */

#ifndef TELEMETRY_PROTO_H
#define TELEMETRY_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */

#define TLM_PROTO_VERSION   0x01u

/* ------------------------------------------------------------------ */
/* Topology                                                            */
/* ------------------------------------------------------------------ */

#define TLM_NODE_COUNT      3u   /* number of Bluepill CAN nodes        */
#define TLM_CH_PER_NODE     3u   /* analog channels each node reports   */

/* ------------------------------------------------------------------ */
/* CAN layer                                                           */
/* ------------------------------------------------------------------ */
/*
 * Bus: 500 kbit/s, standard 11-bit identifiers.
 *
 *   0x100 + node_id   NODE_DATA   DLC 8   sent at TLM_NODE_TX_PERIOD_MS
 *
 * Lower IDs win arbitration, so node 0 has priority. IDs 0x000-0x0FF are left
 * free for future high-priority traffic (faults, sync, command frames).
 */

#define TLM_CAN_BITRATE          500000u
#define TLM_CAN_DATA_BASE_ID     0x100u
#define TLM_CAN_DLC              8u
#define TLM_NODE_TX_PERIOD_MS    100u   /* each node transmits at 10 Hz */

#define TLM_CAN_ID_FOR_NODE(n)   (TLM_CAN_DATA_BASE_ID + (uint32_t)(n))
#define TLM_CAN_NODE_FROM_ID(id) ((uint8_t)((id) - TLM_CAN_DATA_BASE_ID))
#define TLM_CAN_ID_IS_NODE(id)   ((id) >= TLM_CAN_DATA_BASE_ID && \
                                  (id) <  TLM_CAN_DATA_BASE_ID + TLM_NODE_COUNT)

/*
 * NODE_DATA payload, 8 bytes:
 *
 *   [0..1]  ch0     int16   raw counts, see TLM_CHANNELS[] for scale/unit
 *   [2..3]  ch1     int16
 *   [4..5]  ch2     int16
 *   [6]     seq     uint8   rolling 0..255, increments once per transmission
 *   [7]     status  uint8   TLM_ST_* bitfield
 *
 * seq lets the hub detect frames lost on the CAN bus without timestamps.
 */

#define TLM_ST_OK            0x00u
#define TLM_ST_SENSOR_FAULT  0x01u  /* node could not read a sensor         */
#define TLM_ST_STARTUP       0x02u  /* first few seconds after reset        */
#define TLM_ST_CAN_ERROR     0x04u  /* node saw bus errors / recovered      */

typedef struct {
    int16_t ch[TLM_CH_PER_NODE];
    uint8_t seq;
    uint8_t status;
} tlm_node_sample_t;

/* ------------------------------------------------------------------ */
/* LoRa / USB-serial frame (hub -> PC), and the SD card log format     */
/* ------------------------------------------------------------------ */
/*
 * The hub keeps a latest-value table fed by CAN and emits one snapshot of all
 * three nodes every TLM_FRAME_PERIOD_MS. Raw CAN forwarding is NOT possible:
 * the E22 at its default 2.4 kbps air rate gives ~200 usable bytes/second,
 * while the bus itself carries 3 nodes x 10 Hz x 8 bytes = 240 B/s of payload
 * alone. Snapshotting decouples bus rate from radio rate.
 *
 * Frame layout (42 bytes total):
 *
 *   off  size  field
 *   ---  ----  -----------------------------------------------------
 *    0     2   sync    0xA5 0x5A
 *    2     1   ver     TLM_PROTO_VERSION
 *    3     1   len     payload length in bytes (excludes header + CRC)
 *    4     2   seq     uint16  hub frame counter, wraps
 *    6     4   t_ms    uint32  hub uptime in milliseconds
 *   10    30   payload 3 x tlm_node_record_t
 *   40     2   crc16   CRC-16/CCITT-FALSE over bytes [2 .. 39]
 *
 * The CRC deliberately covers ver/len/seq/t_ms as well as the payload, so a
 * corrupted length field cannot be silently accepted. Sync bytes are excluded
 * because they are constant.
 *
 * Because this is a raw byte stream with no escaping, a receiver that loses
 * sync must scan forward for 0xA5 0x5A and validate the CRC before trusting a
 * frame. The payload may legitimately contain the sync pattern; the CRC is
 * what makes false locks vanishingly unlikely.
 */

#define TLM_SYNC0             0xA5u
#define TLM_SYNC1             0x5Au

#define TLM_FRAME_PERIOD_MS   500u   /* 2 Hz -> ~84 bytes/s over the air */

#define TLM_REC_SIZE          10u
#define TLM_HDR_SIZE          10u
#define TLM_CRC_SIZE          2u
#define TLM_PAYLOAD_SIZE      (TLM_NODE_COUNT * TLM_REC_SIZE)               /* 30 */
#define TLM_FRAME_SIZE        (TLM_HDR_SIZE + TLM_PAYLOAD_SIZE + TLM_CRC_SIZE) /* 42 */

/* Offsets, useful when poking at a buffer directly. */
#define TLM_OFF_SYNC0   0u
#define TLM_OFF_SYNC1   1u
#define TLM_OFF_VER     2u
#define TLM_OFF_LEN     3u
#define TLM_OFF_SEQ     4u
#define TLM_OFF_TMS     6u
#define TLM_OFF_PAYLOAD 10u

/*
 * Per-node record, 10 bytes:
 *
 *   [0]     node_id  uint8
 *   [1]     flags    uint8   TLM_NF_* bitfield
 *   [2]     can_seq  uint8   last sequence number seen from this node
 *   [3]     loss     uint8   frames missed since the previous snapshot,
 *                            saturating at 255
 *   [4..5]  ch0      int16
 *   [6..7]  ch1      int16
 *   [8..9]  ch2      int16
 */

#define TLM_NF_ONLINE  0x01u  /* heard from this node since the last snapshot */
#define TLM_NF_STALE   0x02u  /* values are older than TLM_NODE_TIMEOUT_MS    */
#define TLM_NF_FAULT   0x04u  /* node reported TLM_ST_SENSOR_FAULT            */

#define TLM_NODE_TIMEOUT_MS  1500u  /* 15 missed transmissions -> offline */

typedef struct {
    uint8_t node_id;
    uint8_t flags;
    uint8_t can_seq;
    uint8_t loss;
    int16_t ch[TLM_CH_PER_NODE];
} tlm_node_record_t;

typedef struct {
    uint16_t          seq;
    uint32_t          t_ms;
    tlm_node_record_t nodes[TLM_NODE_COUNT];
} tlm_frame_t;

/* ------------------------------------------------------------------ */
/* Channel descriptors                                                 */
/* ------------------------------------------------------------------ */
/*
 * engineering_value = raw * scale + offset
 *
 * Edit TLM_CHANNELS in telemetry_proto.c (and CHANNELS in proto.py) when you
 * swap the simulated data for real sensors. Nothing else needs to change.
 */

typedef struct {
    const char *name;
    const char *unit;
    float       scale;
    float       offset;
} tlm_chan_def_t;

extern const tlm_chan_def_t TLM_CHANNELS[TLM_NODE_COUNT][TLM_CH_PER_NODE];

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. */
uint16_t tlm_crc16(const uint8_t *data, size_t len);

/** Pack a sample into the 8 CAN data bytes. @p d8 must hold 8 bytes. */
void tlm_can_pack(const tlm_node_sample_t *s, uint8_t *d8);

/** Unpack 8 CAN data bytes into a sample. */
void tlm_can_unpack(const uint8_t *d8, tlm_node_sample_t *s);

/**
 * Serialise a frame. Returns TLM_FRAME_SIZE on success, 0 if out_sz is too
 * small. @p out receives the complete on-wire frame including sync and CRC.
 */
size_t tlm_encode_frame(const tlm_frame_t *f, uint8_t *out, size_t out_sz);

/**
 * Parse exactly one frame from the start of @p buf.
 * Returns 0 on success, negative on failure:
 *   -1 too short   -2 bad sync   -3 bad version
 *   -4 bad length  -5 bad CRC
 * Use tlm_find_frame() first if you are reading from a stream.
 */
int tlm_decode_frame(const uint8_t *buf, size_t len, tlm_frame_t *out);

/**
 * Scan @p buf for the first byte offset at which a complete, CRC-valid frame
 * begins. Returns the offset, or -1 if none found. Bytes before the returned
 * offset can be discarded.
 */
int tlm_find_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_PROTO_H */
