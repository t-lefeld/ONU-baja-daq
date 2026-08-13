/**
 * gps_neo_m8n.h - Position/speed/heading from a u-blox NEO-M8N GPS module.
 *
 * Maps to simulation/vehicle_data.py channels: gps_lat, gps_lon, gps_speed,
 * gps_heading, gps_sats (Hub node).
 *
 * The NEO-M8N ships talking NMEA 0183 over UART at 9600 baud by default
 * (configurable higher via u-center, not required for this to work). This
 * driver parses two sentence types out of the stream it's given:
 *   - GGA: fix quality, latitude, longitude, satellite count, altitude
 *   - RMC: speed over ground (converted from knots to mph) and heading
 *
 * It does NOT own a UART peripheral directly - call gps_feed_byte() for
 * every byte the UART RX interrupt hands you (or gps_feed() with a chunk),
 * and it accumulates a line internally, parsing complete sentences as they
 * arrive. This keeps it interrupt-callback friendly without this file
 * needing to know which UART instance or HAL callback naming convention the
 * project uses.
 *
 * TODO before wiring: confirm which USART peripheral/pins the module's TX
 * pin lands on (only the module's TX -> your RX matters for this read-only
 * driver; module RX only matters if you want to send config commands, not
 * needed for basic NMEA output), and wire the antenna with a clear sky view
 * - GPS will not get a fix indoors, so bring-up testing needs to happen
 * outside or near a window.
 */

#ifndef GPS_NEO_M8N_H
#define GPS_NEO_M8N_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double  lat_deg;
    double  lon_deg;
    float   speed_mph;
    float   heading_deg;
    uint8_t satellites;
    uint8_t fix_quality;   /* 0 = no fix, see NMEA GGA fix quality field */
    bool    has_fix;

    /* internal line-accumulation state - do not touch directly */
    char    linebuf[96];
    uint8_t linelen;
} gps_state_t;

void gps_init(gps_state_t *g);

/** Feed one byte from the UART RX stream (e.g. from an interrupt callback). */
void gps_feed_byte(gps_state_t *g, uint8_t byte);

/** Convenience wrapper to feed a whole chunk at once (e.g. from a DMA buffer). */
void gps_feed(gps_state_t *g, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* GPS_NEO_M8N_H */
