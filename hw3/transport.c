/*
 * transport.c
 *
 * CS244a HW#3 (Reliable Transport)
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file.
 *
 */

#include "transport.h"
#include "mysock.h"
#include "stcp_api.h"
#include <assert.h>
#include <math.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// todo: remove after completion
#include <linux/time.h>

#define DEFAULT_WIN_SIZE 3072

enum
{
    CSTATE_ESTABLISHED,
    CSTATE_FIN_WAIT_1,
    CSTATE_FIN_WAIT_2,
    CSTATE_CLOSING,
    CSTATE_TIME_WAIT,
    CSTATE_CLOSE_WAIT,
    CSTATE_LAST_ACK
}; /* obviously you should have more states */

typedef struct
{
    tcp_seq seq;            // 시작 시퀀스 번호
    uint8_t data[STCP_MSS]; // 데이터 내용
    size_t len;             // 유효한 payload 길이
} recv_entry_t;

/* this structure is global to a mysocket descriptor */
typedef struct
{
    /* Connection state */
    bool_t done;          /* TRUE once connection is closed */
    int connection_state; /* state of the connection (established, etc.) */

    /* initital state */
    tcp_seq initial_sequence_num;
    tcp_seq peer_initial_seq;

    /* send window */
    tcp_seq next_seq_num;
    tcp_seq send_base;

    /* recv window */
    tcp_seq receive_next; // 다음으로 기대하는 시퀀스 번호 (in-order 수신 기준)
    recv_entry_t recv_window[(DEFAULT_WIN_SIZE / STCP_MSS) + 1]; // 수신 버퍼

    /*
        For variable RTO: use Karn-Partridge algorithm
        Init:
            RTO = 1.0
            estRTT = -1
            devRTT = 0
            α = 0.125
            β = 0.25

        Send:
            send_time = time_now()
            retransmitted = false

        Timeout:
            RTO = RTO * 2
            retransmitted = true

        ACK received for base:
            if retransmitted == false:
                sampleRTT = time_now() - send_time

                if estRTT == -1:
                    estRTT = sampleRTT
                    devRTT = sampleRTT / 2
                else:
                    estRTT = (1 - α) * estRTT + α * sampleRTT
                    devRTT = (1 - β) * devRTT + β * |sampleRTT - estRTT|

                RTO = estRTT + 4 * devRTT

            retransmitted = false
    */
    struct timespec last_sent_time;
    int retransmission_count;

    double est_rtt;
    double dev_rtt;
    double rto;

    /* Timeout var for send_base */
    struct timespec timeout;
} context_t;

/* static function forward declaration */

/* Utility functions */
static void generate_initial_seq_num (context_t *ctx);
static void update_rto (context_t *ctx);
static void log_time_and_calculate_timeout (context_t *ctx);
static void build_header (STCPHeader *hdr, tcp_seq seq, tcp_seq ack,
                          uint8_t flags);
static bool_t is_valid_segment (const STCPHeader *hdr, ssize_t len,
                                uint8_t expected_flags);

/* core logic */
static int do_active_handshake (mysocket_t sd, context_t *ctx);
static int do_passive_handshake (mysocket_t sd, context_t *ctx);
static void control_loop (mysocket_t sd, context_t *ctx);

/* utility function impl */

/* generate initial sequence number for an STCP connection */
static void
generate_initial_seq_num (context_t *ctx)
{
    assert (ctx);
    ctx->initial_sequence_num = 1;
}

static void
update_rto (context_t *ctx)
{
    assert (ctx);
    struct timespec now;
    clock_gettime (CLOCK_REALTIME, &now);

    double sample = (now.tv_sec - ctx->last_sent_time.tv_sec)
                    + (now.tv_nsec - ctx->last_sent_time.tv_nsec) / 1e9;

    if (ctx->est_rtt < 0) // initial sampling
    {

        ctx->est_rtt = sample;
        ctx->dev_rtt = sample / 2;
    }
    else
    {
        const double alpha = 0.125;
        const double beta = 0.25;
        ctx->est_rtt = (1 - alpha) * ctx->est_rtt + alpha * sample;
        ctx->dev_rtt
            = (1 - beta) * ctx->dev_rtt + beta * fabs (sample - ctx->est_rtt);
    }

    ctx->rto = ctx->est_rtt + 4 * ctx->dev_rtt;
}

static void
log_time_and_calculate_timeout (context_t *ctx)
{
    assert (ctx);
    struct timespec now;
    clock_gettime (CLOCK_REALTIME, &now);

    ctx->last_sent_time = now; // log the send_time

    /* calculate timeout */
    ctx->timeout = now;
    ctx->timeout.tv_sec += (int)ctx->rto;
    ctx->timeout.tv_nsec += (ctx->rto - (int)ctx->rto) * 1e9;
    if (ctx->timeout.tv_nsec >= 1e9) // defensive
    {
        ctx->timeout.tv_sec++;
        ctx->timeout.tv_nsec -= 1e9;
    }
}

static void
build_header (STCPHeader *hdr, tcp_seq seq, tcp_seq ack, uint8_t flags)
{
    memset (hdr, 0, sizeof (*hdr));
    hdr->th_seq = htonl (seq);
    hdr->th_ack = htonl (ack);
    hdr->th_off = sizeof (STCPHeader) / 4;
    hdr->th_flags = flags;
    hdr->th_win = htons (DEFAULT_WIN_SIZE);
}

static bool_t
is_valid_segment (const STCPHeader *hdr, ssize_t len, uint8_t expected_flags)
{
    if (len < (ssize_t)sizeof (STCPHeader))
        return FALSE;
    if ((hdr->th_flags & expected_flags) != expected_flags)
        return FALSE;
    return TRUE;
}

/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void
transport_init (mysocket_t sd, bool_t is_active)
{
    context_t *ctx;

    ctx = (context_t *)calloc (1, sizeof (context_t));
    assert (ctx);

    generate_initial_seq_num (ctx);

    /* XXX: you should send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).  you may also use
     * this to communicate an error condition back to the application, e.g.
     * if connection fails; to do so, just set errno appropriately (e.g. to
     * ECONNREFUSED, etc.) before calling the function.
     */

    ctx->next_seq_num = ctx->initial_sequence_num;
    ctx->send_base = ctx->initial_sequence_num;
    ctx->rto = 1.0;
    ctx->est_rtt = -1.0; // No sampleRTT yet
    ctx->dev_rtt = 0.0;
    ctx->done = FALSE;

    int result = is_active ? do_active_handshake (sd, ctx)
                           : do_passive_handshake (sd, ctx);

    if (result < 0)
    {
        errno = ECONNREFUSED;
        stcp_unblock_application (sd);
        free (ctx);
        return;
    }

    stcp_set_context (sd, ctx); // ctx->stcp_state isn't used in the skeleton,,
                                // but for code consistency
    ctx->connection_state = CSTATE_ESTABLISHED;
    stcp_unblock_application (sd);
    control_loop (sd, ctx);

    /* do any cleanup here */
    free (ctx);
}

/* Core logic implementation */
static int
do_active_handshake (mysocket_t sd, context_t *ctx)
{
    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];

    /* send seq = 1 */
    STCPHeader syn;
    build_header (&syn, ctx->next_seq_num, 0, TH_SYN);

    ctx->retransmission_count = 0;
    while (ctx->retransmission_count < 6)
    {
        stcp_network_send (sd, &syn, sizeof (STCPHeader), NULL);

        if (ctx->retransmission_count == 0)
            ctx->next_seq_num++; // SYN sent, increment seq_num

        log_time_and_calculate_timeout (ctx);

        unsigned int event
            = stcp_wait_for_event (sd, NETWORK_DATA, &ctx->timeout);

        /* timeout case */
        if (!(event & NETWORK_DATA))
        {
            ctx->rto *= 2;
            ctx->retransmission_count++;
            continue;
        }

        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        STCPHeader *synack = (STCPHeader *)buf;

        if (!is_valid_segment (synack, n, TH_SYN | TH_ACK))
        {
            ctx->retransmission_count++;
            continue;
        }

        /* Check Ack num == base + 1 */
        tcp_seq peer_ack = ntohl (synack->th_ack);

        if (peer_ack != ctx->send_base + 1)
        {
            ctx->retransmission_count++;
            continue;
        }

        if (ctx->retransmission_count == 0)
            update_rto (ctx);

        /* here: ack for syn received, update send_base */
        ctx->send_base = peer_ack;
        ctx->peer_initial_seq = ntohl (synack->th_seq);
        ctx->receive_next = ctx->peer_initial_seq + 1;
        STCPHeader ack;
        build_header (&ack, ctx->next_seq_num, ctx->receive_next, TH_ACK);

        stcp_network_send (sd, &ack, sizeof (STCPHeader), NULL);

        return 0;
    }

    return -1;
}

static int
do_passive_handshake (mysocket_t sd, context_t *ctx)
{
    /* receive buffer */
    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];

    /* receive SYN */
    STCPHeader *syn;
    while (1)
    {
        stcp_wait_for_event (sd, NETWORK_DATA, NULL);

        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        syn = (STCPHeader *)buf;

        if (is_valid_segment (syn, n, TH_SYN))
        {
            ctx->peer_initial_seq = ntohl (syn->th_seq);
            ctx->receive_next = ctx->peer_initial_seq + 1;
            break;
        }
    }

    /* send SYNACK */
    STCPHeader synack;
    build_header (&synack, ctx->next_seq_num, ctx->receive_next,
                  TH_SYN | TH_ACK);

    ctx->retransmission_count = 0;
    while (ctx->retransmission_count < 6)
    {
        stcp_network_send (sd, &synack, sizeof (STCPHeader), NULL);

        if (ctx->retransmission_count == 0)
            ctx->next_seq_num++; // SYN sent, increment seq_num

        log_time_and_calculate_timeout (ctx);

        unsigned int event
            = stcp_wait_for_event (sd, NETWORK_DATA, &ctx->timeout);

        /* timeout case */
        if (!(event & NETWORK_DATA))
        {
            ctx->rto *= 2;
            ctx->retransmission_count++;
            continue;
        }

        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        STCPHeader *ack = (STCPHeader *)buf;

        if (!is_valid_segment (ack, n, TH_ACK))
        {
            ctx->retransmission_count++;
            continue;
        }

        /* Check Ack num == base */
        tcp_seq peer_ack = ntohl (ack->th_ack);

        if (peer_ack != ctx->send_base + 1)
        {
            ctx->retransmission_count++;
            continue;
        }

        if (ctx->retransmission_count == 0)
            update_rto (ctx);

        /* here: ack for synack received, update send_base */
        ctx->send_base = peer_ack;
        tcp_seq peer_seq_start = ntohl (ack->th_seq);
        int header_len = ack->th_off * 4;
        int payload_len = n - header_len;

        ctx->receive_next = peer_seq_start + payload_len;
        return 0;
    }

    return -1;
}

/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void
control_loop (mysocket_t sd, context_t *ctx)
{
    assert (ctx);

    while (!ctx->done)
    {
        unsigned int event;

        /* see stcp_api.h or stcp_api.c for details of this function */
        /* XXX: you will need to change some of these arguments! */
        event = stcp_wait_for_event (sd, 0, NULL);

        /* check whether it was the network, app, or a close request */
        if (event & APP_DATA)
        {
            /* the application has requested that data be sent */
            /* see stcp_app_recv() */
        }

        /* etc. */
    }
}

/**********************************************************************/
/* our_dprintf
 *
 * Send a formatted message to stdout.
 *
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void
our_dprintf (const char *format, ...)
{
    va_list argptr;
    char buffer[1024];

    assert (format);
    va_start (argptr, format);
    vsnprintf (buffer, sizeof (buffer), format, argptr);
    va_end (argptr);
    fputs (buffer, stdout);
    fflush (stdout);
}
