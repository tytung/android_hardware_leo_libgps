/******************************************************************************
 * GPS HAL (hardware abstraction layer) for HD2/Leo
 * 
 * leo-gps.c
 * 
 * Copyright (C) 2006-2009 The Android Open Source Project
 * Copyright (C) 2009-2010 The XDAndroid Project
 * Copyright (C) 2010      dan1j3l @ xda-developers
 * Copyright (C) 2011      tytung  @ xda-developers
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 ******************************************************************************/

#include <errno.h>
#include <semaphore.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <cutils/log.h>
#include <cutils/sockets.h>
#include <gps.h>

#define  LOG_TAG  "gps_leo"

#define  XTRA_BLOCK_SIZE  400
#define  ENABLE_NMEA 1

#define  DUMP_DATA  0
#define  GPS_DEBUG  1

#if GPS_DEBUG
#  define  D(...)   LOGD(__VA_ARGS__)
#else
#  define  D(...)   ((void)0)
#endif

#if ENABLE_NMEA
/* Since NMEA parser requires lcoks */
#define GPS_STATE_LOCK_FIX(_s)           \
{                                        \
    int ret;                             \
    do {                                 \
        ret = sem_wait(&(_s)->fix_sem);  \
    } while (ret < 0 && errno == EINTR); \
}

#define GPS_STATE_UNLOCK_FIX(_s)         \
    sem_post(&(_s)->fix_sem)

static void *gps_timer_thread( void*  arg );
#endif

static void *gps_get_position_thread( void*  arg );

static pthread_mutex_t get_position_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t get_position_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t get_pos_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t get_pos_ready_cond = PTHREAD_COND_INITIALIZER;

static int started = 0;
static int active = 0;

void update_gps_location(GpsLocation *location);
void update_gps_status(GpsStatusValue value);
void update_gps_svstatus(GpsSvStatus *svstatus);
void update_gps_nmea(GpsUtcTime timestamp, const char* nmea, int length);

extern uint8_t get_cleanup_value();
extern uint8_t get_precision_value();

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   T O K E N I Z E R                     *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* this is the state of our connection */

typedef struct {
    const char*  p;
    const char*  end;
} Token;

#define  MAX_NMEA_TOKENS  32

typedef struct {
    int     count;
    Token   tokens[ MAX_NMEA_TOKENS ];
} NmeaTokenizer;

static int
nmea_tokenizer_init( NmeaTokenizer*  t, const char*  p, const char*  end )
{
    int    count = 0;
    char*  q;

    // the initial '$' is optional
    if (p < end && p[0] == '$')
        p += 1;

    // remove trailing newline
    if (end > p && end[-1] == '\n') {
        end -= 1;
        if (end > p && end[-1] == '\r')
            end -= 1;
    }

    // get rid of checksum at the end of the sentecne
    if (end >= p+3 && end[-3] == '*') {
        end -= 3;
    }

    while (p < end) {
        const char*  q = p;

        q = memchr(p, ',', end-p);
        if (q == NULL)
            q = end;

         if (count < MAX_NMEA_TOKENS) {
             t->tokens[count].p   = p;
             t->tokens[count].end = q;
             count += 1;
         }
        if (q < end)
            q += 1;

        p = q;
    }

    t->count = count;
    return count;
}

static Token
nmea_tokenizer_get( NmeaTokenizer*  t, int  index )
{
    Token  tok;
    static const char*  dummy = "";

    if (index < 0 || index >= t->count) {
        tok.p = tok.end = dummy;
    } else
        tok = t->tokens[index];

    return tok;
}

static int
str2int( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;

    if (len == 0) {
        return -1;
    }

    for ( ; len > 0; len--, p++ )
    {
        int  c;

        if (p >= end)
            goto Fail;

        c = *p - '0';
        if ((unsigned)c >= 10)
            goto Fail;

        result = result*10 + c;
    }
    return  result;

Fail:
    return -1;
}

static double
str2float( const char*  p, const char*  end )
{
    int   result = 0;
    int   len    = end - p;
    char  temp[16];

    if (len == 0) {
        return -1.0;
    }

    if (len >= (int)sizeof(temp))
        return 0.;

    memcpy( temp, p, len );
    temp[len] = 0;
    return strtod( temp, NULL );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       N M E A   P A R S E R                           *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

#define  NMEA_MAX_SIZE  255

typedef struct {
    int      pos;
    int      overflow;
    int      utc_year;
    int      utc_mon;
    int      utc_day;
    int      utc_diff;
    GpsLocation fix;
    GpsSvStatus sv_status;
    int      sv_status_changed;
    uint16_t fix_flags_cached;
    char     in[ NMEA_MAX_SIZE+1 ];
} NmeaReader;

enum {
    STATE_QUIT  = 0,
    STATE_INIT  = 1,
    STATE_START = 2
};

typedef struct {
    int                     init;
    int                     fd;
    GpsCallbacks            callbacks;
    GpsXtraCallbacks        xtra_callbacks;
    AGpsCallbacks           agps_callbacks;
    GpsStatus               status;
    pthread_t               thread;
    pthread_t               pos_thread;
#if ENABLE_NMEA
    pthread_t               tmr_thread;
    sem_t                   fix_sem;
#endif
    int                     fix_freq;
    int                     control[2];
    NmeaReader              reader;
} GpsState;

static GpsState  _gps_state[1];

static void
nmea_reader_update_utc_diff( NmeaReader*  r )
{
    time_t         now = time(NULL);
    struct tm      tm_local;
    struct tm      tm_utc;
    long           time_local, time_utc;

    gmtime_r( &now, &tm_utc );
    localtime_r( &now, &tm_local );

    time_local = mktime(&tm_local);
    time_utc = mktime(&tm_utc);

    r->utc_diff = time_local - time_utc;
    D("%s() is called. utc_diff = %d", __FUNCTION__, r->utc_diff);
}

static void
nmea_reader_init( NmeaReader*  r )
{
    D("%s() is called", __FUNCTION__);
    memset( r, 0, sizeof(*r) );

    r->fix_flags_cached = 0;
    r->pos      = 0;
    r->overflow = 0;
    r->utc_year = -1;
    r->utc_mon  = -1;
    r->utc_day  = -1;

    nmea_reader_update_utc_diff( r );
}

static int
nmea_reader_update_time( NmeaReader*  r, Token  tok )
{
    int        hour, minute;
    double     seconds;
    struct tm  tm;
    time_t     fix_time;

    if (tok.p + 6 > tok.end)
        return -1;

    if (r->utc_year < 0) {
        // no date yet, get current one
        time_t  now = time(NULL);
        gmtime_r( &now, &tm );
        r->utc_year = tm.tm_year + 1900;
        r->utc_mon  = tm.tm_mon + 1;
        r->utc_day  = tm.tm_mday;
    }

    hour    = str2int(tok.p,   tok.p+2);
    minute  = str2int(tok.p+2, tok.p+4);
    seconds = str2float(tok.p+4, tok.end);

    tm.tm_hour  = hour;
    tm.tm_min   = minute;
    tm.tm_sec   = (int) seconds;
    tm.tm_year  = r->utc_year - 1900;
    tm.tm_mon   = r->utc_mon - 1;
    tm.tm_mday  = r->utc_day;
    tm.tm_isdst = 0;

    fix_time = mktime( &tm ) + r->utc_diff;

#if DUMP_DATA
    D("fix_time=%d", fix_time); // UTC time + utc_diff
#endif

    r->fix.timestamp = (long long)fix_time * 1000 + (int)(seconds*1000)%1000;;
    return 0;
}

static int
nmea_reader_update_date( NmeaReader*  r, Token  date, Token  time )
{
    Token  tok = date;
    int    day, mon, year;

    if (tok.p + 6 != tok.end) {
        D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    day  = str2int(tok.p, tok.p+2);
    mon  = str2int(tok.p+2, tok.p+4);
    year = str2int(tok.p+4, tok.p+6) + 2000;

    if ((day|mon|year) < 0) {
        D("date not properly formatted: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }

    r->utc_year  = year;
    r->utc_mon   = mon;
    r->utc_day   = day;

    return nmea_reader_update_time( r, time );
}

static double
convert_from_hhmm( Token  tok )
{
    double  val     = str2float(tok.p, tok.end);
    int     degrees = (int)(floor(val) / 100);
    double  minutes = val - degrees*100.;
    double  dcoord  = degrees + minutes / 60.0;
    return dcoord;
}

static int
nmea_reader_update_latlong( NmeaReader*  r,
                            Token        latitude,
                            char         latitudeHemi,
                            Token        longitude,
                            char         longitudeHemi )
{
    double   lat, lon;
    Token    tok;

    tok = latitude;
    if (tok.p + 6 > tok.end) {
        D("latitude is too short: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    lat = convert_from_hhmm(tok);
    if (latitudeHemi == 'S')
        lat = -lat;

    tok = longitude;
    if (tok.p + 6 > tok.end) {
        D("longitude is too short: '%.*s'", tok.end-tok.p, tok.p);
        return -1;
    }
    lon = convert_from_hhmm(tok);
    if (longitudeHemi == 'W')
        lon = -lon;

    r->fix.flags    |= GPS_LOCATION_HAS_LAT_LONG;
    r->fix.latitude  = lat;
    r->fix.longitude = lon;
    return 0;
}

static int
nmea_reader_update_altitude( NmeaReader*  r,
                             Token        altitude,
                             Token        units,
                             Token        geoid_height )
{
    /*
     * Height can be measured in two ways.
     * The altitude we get from NMEA is H.
     * The altitude in gps.h is defined as h.
     * So the required output must be h = H + N.
     * 
     * h: Height (h) above the WGS84 reference ellipsoid.
     * H: Height (H) above Geoid (mean sea level).
     * N: Height of Geoid (mean sea level) above the WGS84 ellipsoid.
     */
    if (altitude.p >= altitude.end)
        return -1;

    if (geoid_height.p >= geoid_height.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_ALTITUDE;
    r->fix.altitude = str2float(altitude.p, altitude.end) + str2float(geoid_height.p, geoid_height.end);
    return 0;
}

static int
nmea_reader_update_accuracy( NmeaReader*  r,
                             Token        accuracy )
{
    Token   tok = accuracy;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_ACCURACY;
    float precision = (float)get_precision_value();
    r->fix.accuracy = (float)str2float(tok.p, tok.end) * precision;
    return 0;
}

static int
nmea_reader_update_bearing( NmeaReader*  r,
                            Token        bearing )
{
    Token   tok = bearing;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_BEARING;
    r->fix.bearing  = (float)str2float(tok.p, tok.end);
    return 0;
}

static int
nmea_reader_update_speed( NmeaReader*  r,
                          Token        speed )
{
    Token   tok = speed;

    if (tok.p >= tok.end)
        return -1;

    r->fix.flags   |= GPS_LOCATION_HAS_SPEED;
    // convert knots into m/sec (1 knot equals 1.852 km/h, 1 km/h equals 3.6 m/s)
    // since 1.852 / 3.6 is an odd value (periodic), we're calculating the quotient on the fly
    // to obtain maximum precision (we don't want 1.9999 instead of 2)
    r->fix.speed    = (float)str2float(tok.p, tok.end) * 1.852 / 3.6;
    return 0;
}

static void
nmea_reader_parse( NmeaReader*  r )
{
   /* we received a complete sentence, now parse it to generate
    * a new GPS fix...
    */
    NmeaTokenizer  tzer[1];
    Token          tok;
    int            report_nmea = 0;

#if DUMP_DATA
    D("Received: %.*s", r->pos, r->in);
#endif
    if (r->pos < 9) {
#if DUMP_DATA
        D("Too short. discarded.");
#endif
        return;
    }

    nmea_tokenizer_init(tzer, r->in, r->in + r->pos);
/*
#if GPS_DEBUG
    {
        int  n;
        D("Found %d tokens", tzer->count);
        for (n = 0; n < tzer->count; n++) {
            Token  tok = nmea_tokenizer_get(tzer,n);
            D("size of %2d: '%d', ptr=%x", n, tok.end-tok.p, tok.p);
            D("%2d: '%.*s'", n, tok.end-tok.p, tok.p);
        }
    }
#endif
*/
    tok = nmea_tokenizer_get(tzer, 0);
    if (tok.p + 5 > tok.end) {
        D("sentence id '%.*s' too short, ignored.", tok.end-tok.p, tok.p);
        return;
    }

    // ignore first two characters.
    tok.p += 2;
    if ( !memcmp(tok.p, "GSV", 3) ) {
        // Satellites in View
        Token  tok_num_svs           = nmea_tokenizer_get(tzer, 3);
        int    num_svs = str2int(tok_num_svs.p, tok_num_svs.end);
        //report_nmea = 1;

        if (num_svs > 0) {
            Token tok_total_sentences= nmea_tokenizer_get(tzer, 1);
            Token tok_sentence_no    = nmea_tokenizer_get(tzer, 2);

            int sentence_no = str2int(tok_sentence_no.p, tok_sentence_no.end);
            int total_sentences = str2int(tok_total_sentences.p, tok_total_sentences.end);
            int curr;
            int i;

            if (sentence_no == 1) {
                r->sv_status_changed = 0;
                r->sv_status.num_svs = 0;
                memset( r->sv_status.sv_list, 0, sizeof(r->sv_status.sv_list) );
            }

            curr = (sentence_no - 1) * 4;
            i = 0;
            while (i < 4 && r->sv_status.num_svs < num_svs) {
                Token  tok_prn       = nmea_tokenizer_get(tzer, i*4 + 4);
                Token  tok_elevation = nmea_tokenizer_get(tzer, i*4 + 5);
                Token  tok_azimuth   = nmea_tokenizer_get(tzer, i*4 + 6);
                Token  tok_snr       = nmea_tokenizer_get(tzer, i*4 + 7);

                float snr = str2float(tok_snr.p, tok_snr.end);
                if (snr > 0) {
                    r->sv_status.sv_list[curr].prn       = str2int(tok_prn.p, tok_prn.end);
                    r->sv_status.sv_list[curr].elevation = str2float(tok_elevation.p, tok_elevation.end);
                    r->sv_status.sv_list[curr].azimuth   = str2float(tok_azimuth.p, tok_azimuth.end);
                    r->sv_status.sv_list[curr].snr       = snr;
                    r->sv_status.num_svs += 1;
                }
#if DUMP_DATA
                D("GSV sentence %2d of %d: prn=%2d", curr+1, num_svs, r->sv_status.sv_list[curr].prn);
#endif
                curr += 1;
                i += 1;
            }

            if (sentence_no == total_sentences) {
                r->sv_status_changed = 1;
            }
        }

    } else if ( !memcmp(tok.p, "GGA", 3) ) {
        // GPS fix
        Token  tok_fix_status        = nmea_tokenizer_get(tzer,6);
        report_nmea = 1;

        // Fix quality: {0 = invalid}, {1 = GPS fix}, ...
        if (tok_fix_status.p[0] > '0') {
            Token  tok_time          = nmea_tokenizer_get(tzer,1);
            Token  tok_latitude      = nmea_tokenizer_get(tzer,2);
            Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,3);
            Token  tok_longitude     = nmea_tokenizer_get(tzer,4);
            Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,5);
            Token  tok_accuracy      = nmea_tokenizer_get(tzer,8);
            Token  tok_altitude      = nmea_tokenizer_get(tzer,9);
            Token  tok_altitudeUnits = nmea_tokenizer_get(tzer,10);
            Token  tok_geoidHeight   = nmea_tokenizer_get(tzer,11);

            nmea_reader_update_time(r, tok_time);
            nmea_reader_update_latlong(r, tok_latitude,
                                          tok_latitudeHemi.p[0],
                                          tok_longitude,
                                          tok_longitudeHemi.p[0]);
            nmea_reader_update_accuracy(r, tok_accuracy);
            nmea_reader_update_altitude(r, tok_altitude, tok_altitudeUnits, tok_geoidHeight);
        }

    } else if ( !memcmp(tok.p, "RMC", 3) ) {
        // Recommended minimum specific GPS/Transit data
        Token  tok_fix_status        = nmea_tokenizer_get(tzer, 2);
        report_nmea = 1;

        // Status: {A = active} or {V = void}
        if (tok_fix_status.p[0] == 'A') {
            Token  tok_time          = nmea_tokenizer_get(tzer,1);
            Token  tok_latitude      = nmea_tokenizer_get(tzer,3);
            Token  tok_latitudeHemi  = nmea_tokenizer_get(tzer,4);
            Token  tok_longitude     = nmea_tokenizer_get(tzer,5);
            Token  tok_longitudeHemi = nmea_tokenizer_get(tzer,6);
            Token  tok_speed         = nmea_tokenizer_get(tzer,7);
            Token  tok_bearing       = nmea_tokenizer_get(tzer,8);
            Token  tok_date          = nmea_tokenizer_get(tzer,9);

            nmea_reader_update_date( r, tok_date, tok_time );
            nmea_reader_update_latlong( r, tok_latitude,
                                           tok_latitudeHemi.p[0],
                                           tok_longitude,
                                           tok_longitudeHemi.p[0] );
            nmea_reader_update_bearing( r, tok_bearing );
            nmea_reader_update_speed  ( r, tok_speed );
        }

    } else if ( !memcmp(tok.p, "GSA", 3) ) {
        // GPS DOP and active satellites.
        Token  tok_fix_status        = nmea_tokenizer_get(tzer, 2);
        report_nmea = 1;
        r->sv_status.used_in_fix_mask = 0ul;

        // {3 = 3D fix}, {2 = 2D fix}, {1 = no fix}
        if (tok_fix_status.p[0] == '3' || tok_fix_status.p[0] == '2') {
            // We have accuracy in GGA
            //Token  tok_accuracy      = nmea_tokenizer_get(tzer, 16);
            //nmea_reader_update_accuracy(r, tok_accuracy);

            int i;
            for (i = 3; i <= 14; ++i) {
                Token  tok_prn       = nmea_tokenizer_get(tzer, i);
                int prn = str2int(tok_prn.p, tok_prn.end);
                if (prn > 0)
                    r->sv_status.used_in_fix_mask |= (1ul << (prn-1));
            }
        }
#if DUMP_DATA
        D("%s: used_in_fix_mask is 0x%x", __FUNCTION__, r->sv_status.used_in_fix_mask);
#endif
        r->sv_status_changed = 1;

    } else {
        tok.p -= 2;
#if DUMP_DATA
        D("unknown sentence '%.*s", tok.end-tok.p, tok.p);
#endif
    }
#if DUMP_DATA
    if (r->fix.flags) {
        char   temp[256];
        char*  p   = temp;
        char*  end = p + sizeof(temp);
        struct tm   utc;

        p += snprintf( p, end-p, "fix" );
        if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
            p += snprintf(p, end-p, " lat=%g lon=%g", r->fix.latitude, r->fix.longitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ALTITUDE) {
            p += snprintf(p, end-p, " altitude=%g", r->fix.altitude);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_SPEED) {
            p += snprintf(p, end-p, " speed=%g", r->fix.speed);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_BEARING) {
            p += snprintf(p, end-p, " bearing=%g", r->fix.bearing);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_ACCURACY) {
            p += snprintf(p, end-p, " accuracy=%g", r->fix.accuracy);
        }
        if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
            time_t time = r->fix.timestamp / 1000;
            p += snprintf(p, end-p, " time=%s", ctime(&time) );
        }
        D("%s", temp);
    }
#endif
    if (report_nmea) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        update_gps_nmea(tv.tv_sec*1000+tv.tv_usec/1000, r->in, r->pos);
        report_nmea = 0;
    }
}

static void
nmea_reader_addc( NmeaReader*  r, int  c )
{
    if (r->overflow) {
        r->overflow = (c != '\n');
        return;
    }

    if (r->pos >= (int) sizeof(r->in)-1 ) {
        r->overflow = 1;
        r->pos      = 0;
        return;
    }

    r->in[r->pos] = (char)c;
    r->pos       += 1;

    if (c == '\n') {
#if ENABLE_NMEA
        GPS_STATE_LOCK_FIX(_gps_state);
        nmea_reader_parse( r );
        GPS_STATE_UNLOCK_FIX(_gps_state);
#endif
        r->pos = 0;
    }
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       C O N N E C T I O N   S T A T E                 *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/* commands sent to the gps thread */
enum {
    CMD_QUIT  = 0,
    CMD_START = 1,
    CMD_STOP  = 2
};

static void gps_state_done( GpsState*  s ) {

    update_gps_status(GPS_STATUS_ENGINE_OFF);

    // tell the thread to quit, and wait for it
    char   cmd = CMD_QUIT;
    int    ret;
    void*  dummy;

    do { ret=write( s->control[0], &cmd, 1 ); }
    while (ret < 0 && errno == EINTR);

    pthread_join(s->thread, &dummy);
    pthread_join(s->pos_thread, &dummy);

    // close the control socket pair
    close( s->control[0] ); s->control[0] = -1;
    close( s->control[1] ); s->control[1] = -1;

    // close connection to the GPS daemon
    close( s->fd ); s->fd = -1;

    s->init = STATE_QUIT;
#if ENABLE_NMEA
    sem_destroy(&s->fix_sem);
#endif
}

static void gps_state_start( GpsState*  s ) {
    // Navigation started.
    update_gps_status(GPS_STATUS_SESSION_BEGIN);

    char  cmd = CMD_START;
    int   ret;

    do { ret=write( s->control[0], &cmd, 1 ); }
    while (ret < 0 && errno == EINTR);

    if (ret != 1)
        D("%s: could not send CMD_START command: ret=%d: %s",
        __FUNCTION__, ret, strerror(errno));
}

static void gps_state_stop( GpsState*  s ) {
    // Navigation ended.
    update_gps_status(GPS_STATUS_SESSION_END);

    char  cmd = CMD_STOP;
    int   ret;

    do { ret=write( s->control[0], &cmd, 1 ); }
    while (ret < 0 && errno == EINTR);

    if (ret != 1)
        D("%s: could not send CMD_STOP command: ret=%d: %s",
        __FUNCTION__, ret, strerror(errno));
}

static int epoll_register( int  epoll_fd, int  fd ) {
    struct epoll_event  ev;
    int                 ret, flags;

    /* important: make the fd non-blocking */
    flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    do {
        ret = epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fd, &ev );
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static int epoll_deregister( int  epoll_fd, int  fd ) {
    int  ret;
    do {
        ret = epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fd, NULL );
    } while (ret < 0 && errno == EINTR);
    return ret;
}

void update_gps_location(GpsLocation *location) {
#if DUMP_DATA
    D("%s(): GpsLocation=%f, %f", __FUNCTION__, location->latitude, location->longitude);
#endif
    GpsState*  state = _gps_state;
    //Should be made thread safe...
    if(state->callbacks.location_cb)
        state->callbacks.location_cb(location);
}

void update_gps_status(GpsStatusValue value) {
    D("%s(): GpsStatusValue=%d", __FUNCTION__, value);
    GpsState*  state = _gps_state;
    //Should be made thread safe...
    state->status.status=value;
    if(state->callbacks.status_cb)
        state->callbacks.status_cb(&state->status);
}

void update_gps_svstatus(GpsSvStatus *svstatus) {
#if DUMP_DATA
    D("%s(): GpsSvStatus.num_svs=%d", __FUNCTION__, svstatus->num_svs);
#endif
    GpsState*  state = _gps_state;
    //Should be made thread safe...
    if(state->callbacks.sv_status_cb)
        state->callbacks.sv_status_cb(svstatus);
}

void update_gps_nmea(GpsUtcTime timestamp, const char* nmea, int length) {
#if DUMP_DATA
    D("%s(): length=%d, NMEA=%.*s", __FUNCTION__, length, length, nmea);
#endif
    GpsState*  state = _gps_state;
    //Should be made thread safe...
    if(state->callbacks.nmea_cb)
        state->callbacks.nmea_cb(timestamp, nmea, length);
}

/* this is the main thread, it waits for commands from gps_state_start/stop and,
 * when started, messages from the NMEA SMD. these are simple NMEA sentences
 * that must be parsed to be converted into GPS fixes sent to the framework
 */
static void* gps_state_thread( void*  arg ) {
    GpsState*   state = (GpsState*) arg;
    NmeaReader  *reader;
    int         epoll_fd   = epoll_create(2);
    int         gps_fd     = state->fd;
    int         control_fd = state->control[1];

    reader = &state->reader;
    nmea_reader_init( reader );

    // register control file descriptors for polling
    epoll_register( epoll_fd, control_fd );
    if (gps_fd > -1) {
        epoll_register( epoll_fd, gps_fd );
    }

    D("gps thread running");

    // now loop
    for (;;) {
        struct epoll_event   events[2];
        int                  ne, nevents;

        nevents = epoll_wait( epoll_fd, events, gps_fd>-1 ? 2 : 1, -1 );
        if (nevents < 0) {
            if (errno != EINTR)
                LOGE("epoll_wait() unexpected error: %s", strerror(errno));
            continue;
        }
        //D("gps thread received %d events", nevents);
        for (ne = 0; ne < nevents; ne++) {
            if ((events[ne].events & (EPOLLERR|EPOLLHUP)) != 0) {
                LOGE("EPOLLERR or EPOLLHUP after epoll_wait() !?");
                goto Exit;
            }
            if ((events[ne].events & EPOLLIN) != 0) {
                int  fd = events[ne].data.fd;

                if (fd == control_fd) {
                    char  cmd = 255;
                    int   ret;
                    //D("gps control fd event");
                    do {
                        ret = read( fd, &cmd, 1 );
                    } while (ret < 0 && errno == EINTR);
    
                    if (cmd == CMD_QUIT) {
                        D("gps thread quitting on demand");
                        active = 0;
                        pthread_cond_signal(&get_pos_ready_cond);
                        pthread_cond_signal(&get_position_cond);
                        goto Exit;
                    } else if (cmd == CMD_START) {
                        if (!started) {
                            D("gps thread starting  location_cb=%p", state->callbacks.location_cb);
                            started = 1;
                            pthread_cond_signal(&get_position_cond);
#if ENABLE_NMEA
                            state->init = STATE_START;
                            if ( pthread_create( &state->tmr_thread, NULL, gps_timer_thread, state ) != 0 ) {
                                LOGE("could not create gps_timer_thread: %s", strerror(errno));
                                started = 0;
                                state->init = STATE_INIT;
                                goto Exit;
                            }
#endif
                       }
                    } else if (cmd == CMD_STOP) {
                        if (started) {
                            D("gps thread stopping");
                            started = 0;
                            pthread_cond_signal(&get_pos_ready_cond);
#if ENABLE_NMEA
                            void*  dummy;
                            state->init = STATE_INIT;
                            pthread_join(state->tmr_thread, &dummy);
#endif
                            exit_gps_rpc();
                        }
                    }
                } else if (fd == gps_fd) {
                    char  buf[512];
                    int   nn, ret;
#if DUMP_DATA
                    D("gps fd event");
#endif
                    do {
                        ret = read( fd, buf, sizeof(buf) );
                    } while (ret < 0 && errno == EINTR);

                    if (ret > 0) {
                        for (nn = 0; nn < ret; nn++) {
                            nmea_reader_addc( reader, buf[nn] );
#if DUMP_DATA & 0
                            D("%2d, nmea_reader_addc() is called", nn+1);
#endif
                        }
                    }
#if DUMP_DATA
                    D("gps fd event end");
#endif
                } else {
                    LOGE("epoll_wait() returned unkown fd %d ?", fd);
                }
            }
        }
    }
Exit:
    return NULL;
}

#if ENABLE_NMEA
static void* gps_timer_thread( void*  arg ) {
    D("%s() running", __FUNCTION__);
    GpsState   *state = (GpsState*) arg;
    NmeaReader *r = &(state->reader);
    r->fix.flags = 0;
    r->fix_flags_cached = 0;
    r->sv_status_changed = 0;
    r->sv_status.num_svs = 0;
    memset( r->sv_status.sv_list, 0, sizeof(r->sv_status.sv_list) );

    do {
        GPS_STATE_LOCK_FIX(state);

#if DUMP_DATA
        D("r->fix.flags = 0x%x", r->fix.flags);
#endif
        if (r->fix.flags & GPS_LOCATION_HAS_LAT_LONG) {
            if (r->fix_flags_cached > 0)
                r->fix.flags |= r->fix_flags_cached;
            r->fix_flags_cached = r->fix.flags;
            update_gps_location( &r->fix );
#if DUMP_DATA
            D("r->fix.flags = 0x%x", r->fix.flags);
#endif
            r->fix.flags = 0;
        }

        if (r->sv_status_changed) {
            update_gps_svstatus( &r->sv_status );
            r->sv_status_changed = 0;
        }

        GPS_STATE_UNLOCK_FIX(state);

        uint64_t microseconds = (state->fix_freq * 1000000) - 500000;
        usleep(microseconds);
        //D("%s() usleep(%ld)", __FUNCTION__, microseconds);

    } while(state->init == STATE_START);

    D("%s() destroyed", __FUNCTION__);
    return NULL;
}
#endif

void pdsm_pd_callback() {
    pthread_cond_signal(&get_pos_ready_cond);
}

static void* gps_get_position_thread( void*  arg ) {
    D("%s() running", __FUNCTION__);
    GpsState*  s = _gps_state;
    while(active)
    {
        while(started)
        {
            gps_get_position();
            pthread_mutex_lock(&get_pos_ready_mutex);
            pthread_cond_wait(&get_pos_ready_cond, &get_pos_ready_mutex);
            pthread_mutex_unlock(&get_pos_ready_mutex);
        }
        pthread_mutex_lock(&get_position_mutex);
        pthread_cond_wait(&get_position_cond, &get_position_mutex);
        pthread_mutex_unlock(&get_position_mutex);
    }
    D("%s() destroyed", __FUNCTION__);
    return NULL;
}

static void gps_state_init( GpsState*  state ) {

    update_gps_status(GPS_STATUS_ENGINE_ON);

    state->init       = STATE_INIT;;
    state->control[0] = -1;
    state->control[1] = -1;
    state->fix_freq   = -1;
#if ENABLE_NMEA
    state->fd         = open("/dev/smd27", O_RDONLY);
#else
    state->fd         = -1;
#endif

    active = 1;

#if ENABLE_NMEA
    if ( sem_init(&state->fix_sem, 0, 1) != 0 ) {
        LOGE("gps semaphore initialization failed: %s", strerror(errno));
        goto Fail;
    }
#endif

    if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, state->control ) < 0 ) {
        LOGE("could not create thread control socket pair: %s", strerror(errno));
        goto Fail;
    }

    if ( pthread_create( &state->thread, NULL, gps_state_thread, state ) != 0 ) {
        LOGE("could not create gps thread: %s", strerror(errno));
        goto Fail;
    }

    if ( pthread_create( &state->pos_thread, NULL, gps_get_position_thread, NULL ) != 0 ) {
        LOGE("could not create gps_get_position_thread: %s", strerror(errno));
        goto Fail;
    }

    if(init_gps_rpc())
        goto Fail;

    D("gps state initialized");
    return;

Fail:
    gps_state_done( state );
}

/*****************************************************************/
/*****************************************************************/
/*****                                                       *****/
/*****       I N T E R F A C E                               *****/
/*****                                                       *****/
/*****************************************************************/
/*****************************************************************/

/***** GpsXtraInterface *****/

static int gps_xtra_init(GpsXtraCallbacks* callbacks) {
    D("%s() is called", __FUNCTION__);
    GpsState*  s = _gps_state;

    s->xtra_callbacks = *callbacks;

    return 0;
}

static int gps_xtra_inject_xtra_data(char* data, int length) {
    D("%s() is called", __FUNCTION__);
    D("gps_xtra_inject_xtra_data: xtra size = %d, data ptr = 0x%x\n", length, (int) data);
    GpsState*  s = _gps_state;
    if (!s->init)
        return 0;

    int rpc_ret_val = -1;
    int ret_val = -1;
    unsigned char *xtra_data_ptr;
    uint32_t  part_len;
    uint8_t   part;
    uint8_t   total_parts;
    uint16_t  len_injected;

    total_parts = (length / XTRA_BLOCK_SIZE);
    if ((total_parts % XTRA_BLOCK_SIZE) != 0)
    {
        total_parts += 1;
    }

    uint8_t part_no = total_parts % 10;
    if (part_no > 0)
        part_no = total_parts - part_no;
    else
        part_no = total_parts - 5;

    len_injected = 0; // O bytes injected
    // XTRA injection starts with part 1
    D("gps_xtra_inject_xtra_data: inject part = %d/%d, len = %d\n", 1, total_parts, XTRA_BLOCK_SIZE);
    D("gps_xtra_inject_xtra_data: ......");
    for (part = 1; part <= total_parts; part++)
    {
        part_len = XTRA_BLOCK_SIZE;
        if (XTRA_BLOCK_SIZE > (length - len_injected))
        {
            part_len = length - len_injected;
        }
        xtra_data_ptr = data + len_injected;

        if (part > part_no) // reduce the number of the xtra debugging info
            D("gps_xtra_inject_xtra_data: inject part = %d/%d, len = %d\n", part, total_parts, part_len);

        if (part < total_parts)
        {
            rpc_ret_val = gps_xtra_set_data(xtra_data_ptr, part_len, part, total_parts);
            if (rpc_ret_val == -1)
            {
                D("gps_xtra_set_data() for xtra returned %d \n", rpc_ret_val);
                ret_val = EINVAL; // return error
                break;
            }
        }
        else // part == total_parts
        {
            ret_val = gps_xtra_set_data(xtra_data_ptr, part_len, part, total_parts);
            break; // done with injection
        }

        len_injected += part_len;
    }

    return ret_val;
}

void xtra_download_request() {
    D("%s() is called", __FUNCTION__);
     GpsState*  state = _gps_state;
     //Should be made thread safe...
    if(state->xtra_callbacks.download_request_cb)
        state->xtra_callbacks.download_request_cb();
}

static const GpsXtraInterface  sGpsXtraInterface = {
    gps_xtra_init,
    gps_xtra_inject_xtra_data,
};

/***** AGpsInterface *****/

static void agps_init(AGpsCallbacks* callbacks) {
    D("%s() is called", __FUNCTION__);
    GpsState*  s = _gps_state;

    s->agps_callbacks = *callbacks;
}

static int agps_data_conn_open(const char* apn) {
    D("%s() is called", __FUNCTION__);
    D("apn=%s", apn);
    /* not yet implemented */
    return 0;
}

static int agps_data_conn_closed() {
    D("%s() is called", __FUNCTION__);
    /* not yet implemented */
    return 0;
}

static int agps_data_conn_failed() {
    D("%s() is called", __FUNCTION__);
    /* not yet implemented */
    return 0;
}

static int agps_set_server(AGpsType type, const char* hostname, int port) {
    D("%s() is called", __FUNCTION__);
    D("type=%d, hostname=%s, port=%d", type, hostname, port);
    /* not yet implemented */
    return 0;
}

static const AGpsInterface  sAGpsInterface = {
    agps_init,
    agps_data_conn_open,
    agps_data_conn_closed,
    agps_data_conn_failed,
    agps_set_server,
};

/***** GpsInterface *****/

static int gps_init(GpsCallbacks* callbacks) {
    D("%s() is called", __FUNCTION__);
    GpsState*  s = _gps_state;

    if (!s->init)
        gps_state_init(s);

    s->callbacks = *callbacks;

    return 0;
}

static void gps_cleanup() {
    D("%s() is called", __FUNCTION__);
    if (get_cleanup_value()) {
        GpsState*  s = _gps_state;

        if (s->init) {
            gps_state_done(s);
            cleanup_gps_rpc_clients();
        }
    }
}

static int gps_start() {
    D("%s: called", __FUNCTION__);

    GpsState*  s = _gps_state;

    if (!s->init) {
        D("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    gps_state_start(s);
    return 0;
}

static int gps_stop() {
    D("%s: called", __FUNCTION__);

    GpsState*  s = _gps_state;

    if (!s->init) {
        D("%s: called with uninitialized state !!", __FUNCTION__);
        return -1;
    }

    gps_state_stop(s);
    return 0;
}

static int gps_inject_time(GpsUtcTime time, int64_t timeReference, int uncertainty) {
    D("%s() is called", __FUNCTION__);
    D("time=%lld, timeReference=%lld, uncertainty=%d", time, timeReference, uncertainty);
    GpsState*  s = _gps_state;
    if (!s->init)
        return 0;

    int ret_val = -1;
    ret_val = gps_xtra_inject_time_info(time, timeReference, uncertainty);
    return ret_val;
}

static int gps_inject_location(double latitude, double longitude, float accuracy) {
    D("%s() is called", __FUNCTION__);
    D("latitude=%f, longitude=%f, accuracy=%f", latitude, longitude, accuracy);
    /* not yet implemented */
    return 0;
}

static void gps_delete_aiding_data(GpsAidingData flags) {
    D("%s() is called", __FUNCTION__);
    D("flags=%d", flags);
    /* not yet implemented */
}

static int gps_set_position_mode(GpsPositionMode mode, int fix_frequency) {
    D("%s() is called", __FUNCTION__);
    D("fix_frequency=%d", fix_frequency);
    GpsState*  s = _gps_state;
    if (!s->init)
        return 0;

    if (fix_frequency == 0) {
        //We don't handle single shot requests atm...
        //So one every 1 seconds will it be.
        fix_frequency = 1;
    } else if (fix_frequency > 1800) { //30mins
        fix_frequency = 1800;
    }
    // fix_frequency is only used by NMEA version
    s->fix_freq = fix_frequency;
    return 0;
}

static const void* gps_get_extension(const char* name) {
    D("%s('%s') is called", __FUNCTION__, name);
    if (!strcmp(name, GPS_XTRA_INTERFACE)) {
        return &sGpsXtraInterface;
    } else if (!strcmp(name, AGPS_INTERFACE)) {
        return &sAGpsInterface;
    }
    return NULL;
}

static const GpsInterface  hardwareGpsInterface = {
    gps_init,
    gps_start,
    gps_stop,
    gps_cleanup,
    gps_inject_time,
    gps_inject_location,
    gps_delete_aiding_data,
    gps_set_position_mode,
    gps_get_extension,
};

const GpsInterface* gps_get_hardware_interface()
{
    D("%s() is called", __FUNCTION__);
    return &hardwareGpsInterface;
}

// END OF FILE
