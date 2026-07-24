#include "nmea.h"

#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* "ddmm.mmmm" (+ hemisphere) -> decimal degrees */
static double coord_to_deg(const char *s, char hemi)
{
    double v = atof(s);
    double deg = (double)((int)(v / 100.0));
    double min = v - deg * 100.0;
    double d = deg + min / 60.0;
    return (hemi == 'S' || hemi == 'W') ? -d : d;
}

/* split on ',' in place; returns field count */
static int split(char *s, char *f[], int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++) {
        if (*s == ',') {
            *s = 0;
            f[n++] = s + 1;
        }
    }
    return n;
}

/* UTC date (ddmmyy) + time (hhmmss[.sss]) -> Unix seconds. Civil
 * calendar, no leap seconds (GPS-UTC offset is irrelevant at our
 * 1 s freshness granularity). Returns 0 if either field is empty. */
static uint32_t rmc_unix(const char *time, const char *date)
{
    if (!date[0] || !time[0]) return 0;
    int dd = (date[0]-'0')*10 + (date[1]-'0');
    int mo = (date[2]-'0')*10 + (date[3]-'0');
    int yy = (date[4]-'0')*10 + (date[5]-'0');
    int hh = (time[0]-'0')*10 + (time[1]-'0');
    int mi = (time[2]-'0')*10 + (time[3]-'0');
    int ss = (time[4]-'0')*10 + (time[5]-'0');
    if (mo < 1 || mo > 12 || dd < 1) return 0;
    int year = 2000 + yy;
    static const int mdays[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    long days = (year - 1970) * 365L + (year - 1969) / 4; /* leap days */
    days += mdays[mo - 1];
    if (mo > 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        days += 1;
    days += dd - 1;
    return (uint32_t)(days * 86400L + hh * 3600 + mi * 60 + ss);
}

static bool parse_rmc(char *line, nmea_fix_t *out)
{
    /* $GxRMC,time,status,lat,N/S,lon,E/W,knots,course,date,... */
    char *f[13];
    int n = split(line, f, 13);
    if (n < 9) return false;

    memset(out, 0, sizeof(*out));
    out->valid = (f[2][0] == 'A');
    if (f[3][0]) out->lat = coord_to_deg(f[3], f[4][0]);
    if (f[5][0]) out->lon = coord_to_deg(f[5], f[6][0]);
    if (f[7][0]) out->speed_mps = (float)(atof(f[7]) * 0.514444);
    if (f[8][0]) out->course_deg = (float)atof(f[8]);
    if (n > 9) out->unix_s = rmc_unix(f[1], f[9]); /* date = field 9 */
    return true;
}

bool nmea_feed_char(nmea_parser_t *p, char c, nmea_fix_t *out)
{
    if (c == '$') { /* sentence start (also resyncs after garbage) */
        p->len = 0;
        p->line[p->len++] = c;
        return false;
    }
    if (p->len == 0) return false; /* waiting for '$' */
    if (p->len >= (int)sizeof(p->line) - 1) {
        p->len = 0; /* overlong: drop and resync */
        return false;
    }

    if (c != '\r' && c != '\n') {
        p->line[p->len++] = c;
        return false;
    }

    /* end of sentence */
    p->line[p->len] = 0;
    int len = p->len;
    p->len = 0;

    /* checksum: $<body>*hh */
    if (len < 9 || p->line[len - 3] != '*') return false;
    int want = hexval(p->line[len - 2]) * 16 + hexval(p->line[len - 1]);
    if (want < 0) return false;
    unsigned cs = 0;
    for (int i = 1; i < len - 3; i++) cs ^= (unsigned char)p->line[i];
    if ((int)cs != want) return false;

    /* talker-agnostic RMC ($GPRMC/$GNRMC/...) */
    if (len < 7 || memcmp(p->line + 3, "RMC,", 4) != 0) return false;

    p->line[len - 3] = 0; /* trim checksum for field splitting */
    return parse_rmc(p->line, out);
}
