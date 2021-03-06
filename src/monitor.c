#include <assert.h>
#include <pthread.h>
#include <pcap/pcap.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

#include "monitor.h"
#include "token_bucket.h"
#include "util.h"
#include "util_time.h"
#include "log.h"

#define PCAP_SAVEFILE_MAGIC 0xa1b2c3d4
#define MONITOR_SNAPLEN     2000
#define MONITOR_BUFSIZE     5*1024*1024 // 5mb

struct pcap_timeval
{
    // seconds
    bpf_int32 tv_sec;
    // microseconds
    bpf_int32 tv_usec;
} __attribute__((__packed__));

struct pcap_sf_pkthdr
{
    // time stamp
    struct pcap_timeval ts;
    // length of portion present
    bpf_u_int32 caplen;
    // length this packet (off wire)
    bpf_u_int32 len;
} __attribute__((__packed__));

struct zmonitor_struct
{
    pthread_rwlock_t lock;
    token_bucket_t band;
    UT_array monitors;
};

struct zmonitor_conn_struct
{
    zmonitor_t *mon;
    struct bufferevent *bev;
    struct bpf_program bpf;
    token_bucket_t band;
    bool has_file_header;
    bool active;
};

/**
 * Create new instance.
 * @param[in] max_bandwidth Max total bandwidth.
 * @return New instance.
 */
zmonitor_t *zmonitor_new(uint64_t max_bandwidth)
{
    zmonitor_t *mon = malloc(sizeof(*mon));

    if (unlikely(NULL == mon)) {
        return NULL;
    }
    memset(mon, 0, sizeof(*mon));

    if (unlikely(0 != pthread_rwlock_init(&mon->lock, NULL))) {
        free(mon);
        return NULL;
    }
    token_bucket_init(&mon->band, max_bandwidth);
    utarray_init(&mon->monitors, &ut_ptr_icd);

    return mon;
}

/**
 * Destroy and free monitor instance.
 * @param[in] mon Monitor instance.
 */
void zmonitor_free(zmonitor_t *mon)
{
    for (size_t i = 0; i < utarray_len(&mon->monitors); i++) {
        zmonitor_conn_t *conn = *(zmonitor_conn_t **) utarray_eltptr(&mon->monitors, i);
        zmonitor_conn_free(conn);
    }
    pthread_rwlock_destroy(&mon->lock);
    token_bucket_destroy(&mon->band);
    utarray_done(&mon->monitors);
    free(mon);
}

/**
 * Create new monitor connection.
 * @param[in] max_bandwidth Maximum connection bandwidth.
 * @return New monitor instance.
 */
zmonitor_conn_t *zmonitor_conn_new(uint64_t max_bandwidth)
{
    zmonitor_conn_t *conn = malloc(sizeof(*conn));
    if (unlikely(!conn)) {
        return NULL;
    }
    memset(conn, 0, sizeof(*conn));
    token_bucket_init(&conn->band, max_bandwidth);

    return conn;
}

/**
 * Destroy and free monitor connection.
 * @param[in] Monitor connection.
 */
void zmonitor_conn_free(zmonitor_conn_t *conn)
{
    if (likely(conn->bev)) {
        bufferevent_free(conn->bev);
    }
    pcap_freecode(&conn->bpf);
    free(conn);
}

/**
 * Activate monitor connection.
 * @param [in] conn Monitor connection.
 * @param[in] mon Monitor instance.
 */
void zmonitor_conn_activate(zmonitor_conn_t *conn, zmonitor_t *mon)
{
    assert(!conn->active);

    pthread_rwlock_wrlock(&mon->lock);
    utarray_push_back(&mon->monitors, &conn);
    conn->active = true;
    conn->mon = mon;
    pthread_rwlock_unlock(&mon->lock);
}

/**
 * Deactivate monitor connection.
 * @param[in] conn Monitor connection.
 */
void zmonitor_conn_deactivate(zmonitor_conn_t *conn)
{
    assert(conn->active);

    if (conn->active) {
        pthread_rwlock_wrlock(&conn->mon->lock);

        zmonitor_conn_t **ptr = (zmonitor_conn_t **) utarray_front(&conn->mon->monitors);
        while (ptr) {
            if (*ptr == conn) {
                ssize_t idx = utarray_eltidx(&conn->mon->monitors, ptr);
                utarray_erase(&conn->mon->monitors, idx, 1);
                conn->active = false;
                break;
            }
            ptr = (zmonitor_conn_t **) utarray_next(&conn->mon->monitors, ptr);
        }

        pthread_rwlock_unlock(&conn->mon->lock);
    }
}

/**
 * Set BPF filter on monitor connection.
 * @param[in] conn Monitor connection.
 * @param[in] filter Filter string.
 * @return True on success.
 */
bool zmonitor_conn_set_filter(zmonitor_conn_t *conn, const char *filter)
{
    assert(!conn->active);

    pcap_t *cap = pcap_open_dead(DLT_EN10MB, MONITOR_SNAPLEN);
    pcap_freecode(&conn->bpf);

    if (0 != pcap_compile(cap, &conn->bpf, filter, 1, PCAP_NETMASK_UNKNOWN)) {
        ZLOG(LOG_INFO, "Invalid monitor filter \"%s\": %s", filter, pcap_geterr(cap));
        pcap_close(cap);
        return false;
    }

    pcap_close(cap);
    return true;
}

/**
 * Event handler for monitor connection.
 * @param[in] bev
 * @param[in] events
 * @param[in] ctx
 */
static void zmonitor_event_cb(struct bufferevent *bev, short events, void *ctx)
{
    zmonitor_conn_t *conn = (zmonitor_conn_t *) ctx;

    if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
        bufferevent_unlock(bev);
        zmonitor_conn_deactivate(conn);
        bufferevent_lock(bev);
        zmonitor_conn_free(conn);
    }
}

/**
 * Link bufferevent with monitor connection.
 * @param[in] conn Monitor connection.
 * @param[in] bev Bufferevent instance.
 */
void zmonitor_conn_set_listener(zmonitor_conn_t *conn, struct bufferevent *bev)
{
    bufferevent_disable(bev, EV_READ);
    bufferevent_setcb(bev, NULL, NULL, zmonitor_event_cb, conn);
    conn->bev = bev;
}

/**
 * Apply filters on packet and send it to listeners on success.
 * @param[in] mon Monitor instance.
 * @param[in] packet Packet buffer.
 * @param[in] len Packet length.
 */
void zmonitor_mirror_packet(zmonitor_t *mon, void *packet, size_t len)
{
    pthread_rwlock_rdlock(&mon->lock);

    if (likely(!utarray_len(&mon->monitors))) {
        goto end;
    }

    ztime_t ts = ztime();
    struct pcap_pkthdr pkthdr = {
            .ts = {0},
            .caplen = (uint32_t) len,
            .len = (uint32_t) len
    };
    struct pcap_sf_pkthdr sf_pkthdr = {
            .ts = {.tv_sec = (uint32_t) USEC2SEC(ts), .tv_usec = (uint32_t) (ts % 1000000)},
            .caplen = (uint32_t) len,
            .len = (uint32_t) len
    };

    zmonitor_conn_t **pconn = (zmonitor_conn_t **) utarray_front(&mon->monitors);
    while (pconn) {
        size_t bufsize = evbuffer_get_length(bufferevent_get_output((*pconn)->bev));
        if (bufsize > MONITOR_BUFSIZE) {
            break;
        }

        if ((0 == (*pconn)->bpf.bf_len) || (0 != pcap_offline_filter(&(*pconn)->bpf, &pkthdr, packet))) {
            if (!token_bucket_update(&(*pconn)->band, len)) {
                break;
            }
            if (!token_bucket_update(&mon->band, len)) {
                token_bucket_rollback(&(*pconn)->band, len);
                break;
            }

            bufferevent_lock((*pconn)->bev);
            if (unlikely(!(*pconn)->has_file_header)) {
                // todo: this code does not work if it placed in monitor_set_listener()
                struct pcap_file_header sf_hdr = {
                        .magic = PCAP_SAVEFILE_MAGIC,
                        .version_major = PCAP_VERSION_MAJOR,
                        .version_minor = PCAP_VERSION_MINOR,
                        .thiszone = 0,
                        .sigfigs = 0,
                        .snaplen = MONITOR_SNAPLEN,
                        .linktype = DLT_EN10MB,
                };
                bufferevent_write((*pconn)->bev, &sf_hdr, sizeof(sf_hdr));
                (*pconn)->has_file_header = true;
            }
            bufferevent_write((*pconn)->bev, &sf_pkthdr, sizeof(sf_pkthdr));
            bufferevent_write((*pconn)->bev, packet, len);
            bufferevent_unlock((*pconn)->bev);
        }
        pconn = (zmonitor_conn_t **) utarray_next(&mon->monitors, pconn);
    }
    end:
    pthread_rwlock_unlock(&mon->lock);
}
