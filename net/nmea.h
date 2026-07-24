/**
 * nmea.h — minimal NMEA-0183 parser for the USB GPS mouse (RMC only:
 * position, validity, speed, course — everything the pose needs).
 * Pure C, no deps; byte-feed API suits any serial source.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    double   lat, lon;   /* decimal degrees, +N/+E */
    float    speed_mps;
    float    course_deg; /* 0 = north, clockwise */
    bool     valid;      /* RMC status == 'A' */
    uint32_t unix_s;     /* UTC from RMC date+time; 0 if date absent */
} nmea_fix_t;

typedef struct {
    char line[128];
    int  len;
} nmea_parser_t;

/* Feed one byte. Returns true when a complete, checksum-valid RMC
 * sentence was parsed; *out is then filled. */
bool nmea_feed_char(nmea_parser_t *p, char c, nmea_fix_t *out);
