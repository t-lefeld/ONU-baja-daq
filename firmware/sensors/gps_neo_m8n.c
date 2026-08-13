/**
 * gps_neo_m8n.c - minimal NMEA GGA/RMC parser. See gps_neo_m8n.h for wiring
 * TODOs. Deliberately does not use strtok() (not reentrant-friendly across
 * multiple driver instances sharing a toolchain) - fields are split in place
 * with a small manual scanner instead.
 */

#include "gps_neo_m8n.h"
#include <string.h>
#include <stdlib.h>

#define KNOTS_PER_MPH 0.868976f

void gps_init(gps_state_t *g)
{
    memset(g, 0, sizeof(*g));
}

/* Splits `line` in place on commas, writing up to `max_fields` pointers into
 * `fields`. Returns the number of fields found. Mutates `line` (commas
 * become NULs), same tradeoff as strtok but instance-local instead of
 * relying on hidden global state. */
static uint8_t split_fields(char *line, char **fields, uint8_t max_fields)
{
    uint8_t count = 0;
    char *p = line;
    fields[count++] = p;

    while (*p != '\0' && count < max_fields)
    {
        if (*p == ',' || *p == '*')
        {
            *p = '\0';
            fields[count++] = p + 1;
        }
        p++;
    }
    return count;
}

/* NMEA lat/lon fields are ddmm.mmmm (lat) or dddmm.mmmm (lon) - degrees and
 * decimal minutes concatenated, not plain decimal degrees. */
static double parse_dm(const char *field, int deg_digits)
{
    if (field[0] == '\0') return 0.0;

    char deg_buf[4] = {0};
    for (int i = 0; i < deg_digits; i++) deg_buf[i] = field[i];
    double degrees = atof(deg_buf);
    double minutes = atof(field + deg_digits);
    return degrees + minutes / 60.0;
}

static void parse_gga(char **f, uint8_t n, gps_state_t *g)
{
    if (n < 8) return;

    double lat = parse_dm(f[1], 2);
    if (f[2][0] == 'S') lat = -lat;

    double lon = parse_dm(f[3], 3);
    if (f[4][0] == 'W') lon = -lon;

    uint8_t fix_quality = (uint8_t)atoi(f[5]);
    uint8_t sats = (uint8_t)atoi(f[6]);

    g->lat_deg      = lat;
    g->lon_deg       = lon;
    g->fix_quality   = fix_quality;
    g->satellites    = sats;
    g->has_fix       = (fix_quality > 0u);
}

static void parse_rmc(char **f, uint8_t n, gps_state_t *g)
{
    if (n < 8) return;

    /* f[1] is status: 'A' = valid, 'V' = warning/no fix. Keep the last
     * speed/heading either way rather than snapping to 0 on a momentary
     * dropout - GGA's fix_quality is the authoritative "do we have a fix"
     * signal already. */
    float speed_knots = atof(f[6]);
    float heading = atof(f[7]);

    g->speed_mph   = speed_knots / KNOTS_PER_MPH;
    g->heading_deg = heading;
}

static void parse_sentence(char *line, uint8_t len, gps_state_t *g)
{
    if (len < 6 || line[0] != '$') return;

    /* Match by the 3-letter sentence type suffix (GGA/RMC) so this works
     * whether the module prefixes talker ID as GP, GN, or GL. */
    char *comma = memchr(line, ',', len);
    if (!comma) return;
    uint8_t id_len = (uint8_t)(comma - line);
    if (id_len < 3) return;
    const char *type = &line[id_len - 3];

    char *fields[20];
    uint8_t n = split_fields(comma + 1, fields, 20);

    if (memcmp(type, "GGA", 3) == 0)
    {
        parse_gga(fields, n, g);
    }
    else if (memcmp(type, "RMC", 3) == 0)
    {
        parse_rmc(fields, n, g);
    }
}

void gps_feed_byte(gps_state_t *g, uint8_t byte)
{
    if (byte == '\n' || byte == '\r')
    {
        if (g->linelen > 0u)
        {
            g->linebuf[g->linelen] = '\0';
            parse_sentence(g->linebuf, g->linelen, g);
            g->linelen = 0u;
        }
        return;
    }

    if (g->linelen < (uint8_t)(sizeof(g->linebuf) - 1u))
    {
        g->linebuf[g->linelen++] = (char)byte;
    }
    else
    {
        /* Line overran the buffer (shouldn't happen for standard NMEA
         * sentences, which are well under 96 chars) - discard and resync
         * on the next line terminator rather than parsing garbage. */
        g->linelen = 0u;
    }
}

void gps_feed(gps_state_t *g, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        gps_feed_byte(g, data[i]);
    }
}
