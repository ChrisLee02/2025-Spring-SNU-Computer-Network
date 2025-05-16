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

#define DEFAULT_WIN_SIZE 3072
#define MAX_SEGMENTS (DEFAULT_WIN_SIZE / STCP_MSS + 1)

#define MIN_RTO 0.01 // 10ms
#define MAX_RTO 30.0 // 30s
#define MAX_RETRANSMISSION_COUNT 5

typedef enum
{
    EVENT_SEND_FIN,
    EVENT_RECV_FIN,
    EVENT_RECV_ACK,
} stcp_event_t;

typedef enum
{
    CSTATE_ESTABLISHED,
    CSTATE_FIN_WAIT_1,
    CSTATE_FIN_WAIT_2,
    CSTATE_CLOSING,
    CSTATE_CLOSE_WAIT,
    CSTATE_LAST_ACK,
    CSTATE_CLOSED
} stcp_state_t;

typedef struct segment
{
    tcp_seq seq;
    size_t payload_len;
    uint8_t data[STCP_MSS];
    bool_t is_FIN;
    struct segment *next;
} segment_t;

typedef struct
{
    segment_t *head;
} segment_list_t;

/* this structure is global to a mysocket descriptor */
typedef struct
{
    /* Connection state */
    bool_t done;                   /* TRUE once connection is closed */
    stcp_state_t connection_state; /* state of the connection */
    bool_t is_APP_CLOSE_QUEUED;    /* true if app close come and not send */
    bool_t is_FIN_SENT;
    bool_t is_FIN_RECEIVED;

    /* initital state */
    tcp_seq initial_sequence_num;
    tcp_seq peer_initial_seq;

    /* send window */
    tcp_seq next_seq_num;
    tcp_seq send_base;
    segment_list_t send_buffer;

    /* recv window */
    tcp_seq receive_next;
    segment_list_t receive_buffer;

    /* Timeout and retransmission */
    struct timespec last_sent_time; // needed to calculate RTO
    int retransmission_count;

    /* RTO state */
    double est_rtt;
    double dev_rtt;
    double rto;

    /* Timeout var for send_base */
    struct timespec timeout;
} context_t;

/* static function forward declaration */

/*  Buffer Management  */
static void segment_list_init (segment_list_t *list);
static void segment_list_clear (segment_list_t *list);
/* -- Receive buffer -- */
static void insert_segment_to_receive_buffer (segment_list_t *receive_buffer,
                                              tcp_seq seq, const uint8_t *data,
                                              size_t payload_len,
                                              bool_t is_fin);
static void flush_in_order_segments_from_receive_buffer (mysocket_t sd,
                                                         context_t *ctx);
/* -- Send buffer -- */
static void enqueue_send_buffer (segment_list_t *send_buffer, tcp_seq seq,
                                 const uint8_t *data, size_t payload_len,
                                 bool_t is_fin);
static void dequeue_send_buffer (segment_list_t *send_buffer, tcp_seq ack);
static void resend_send_buffer (mysocket_t sd, segment_list_t *send_buffer,
                                tcp_seq send_base, tcp_seq receive_next);

/* Utility functions - RTO, Timer, Utility */
static void generate_initial_seq_num (context_t *ctx);
static void update_rto (context_t *ctx, bool_t is_timeout);
static void log_time_and_calculate_timeout (context_t *ctx);
static void build_header (STCPHeader *hdr, tcp_seq seq, tcp_seq ack,
                          uint8_t flags);
static bool_t is_valid_seg_len_and_flags (const STCPHeader *hdr, ssize_t len,
                                          uint8_t expected_flags);
static stcp_state_t transition_state (stcp_state_t state, stcp_event_t event);

/* Handshake logic */
static int do_active_handshake (mysocket_t sd, context_t *ctx);
static int do_passive_handshake (mysocket_t sd, context_t *ctx);

/* Control loop */
static void control_loop (mysocket_t sd, context_t *ctx);
/* Control loop event handlers */
static void handle_app_data (mysocket_t sd, context_t *ctx);
static void handle_network_data (mysocket_t sd, context_t *ctx);
static void handle_app_close (mysocket_t sd, context_t *ctx);
static void handle_timeout (mysocket_t sd, context_t *ctx);

static void handle_ack_from_peer (mysocket_t sd, context_t *ctx, tcp_seq ack);
static void handle_incoming_segment (mysocket_t sd, context_t *ctx,
                                     tcp_seq seq, const uint8_t *payload,
                                     size_t payload_len, bool_t is_fin);

static void
segment_list_init (segment_list_t *list)
{
    list->head = NULL;
}

static void
segment_list_clear (segment_list_t *list)
{
    segment_t *cur = list->head;
    while (cur)
    {
        segment_t *next = cur->next;
        free (cur);
        cur = next;
    }
    list->head = NULL;
}

/* static void
recv_segment_insert_robust (segment_list_t *list, tcp_seq seq,
                            const uint8_t *data, size_t payload_len,
                            bool_t is_fin)
{
    assert (payload_len > 0 || is_fin);

    // [seq_start, seq_end) 인 상황,,,,
    tcp_seq seg_start = seq;
    tcp_seq seg_end = seq + payload_len + (is_fin ? 1 : 0);


        알고리즘을 짜보자
        - 전제: 리스트는 정렬된 상태를 유지하며, overlapping이 없는 상태로
       유지된다.
        - 리스트를 순회한다
        - cur->end <= seq->start 이면 다음 노드로 이동.
        - cur->end > seq->start


} */

/* Receive side buffering */
static void
insert_segment_to_receive_buffer (segment_list_t *receive_buffer, tcp_seq seq,
                                  const uint8_t *data, size_t payload_len,
                                  bool_t is_fin)
{
    assert (payload_len > 0 || is_fin);

    segment_t *prev = NULL;
    segment_t *cur = receive_buffer->head;

    while (cur && cur->seq < seq)
    {
        prev = cur;
        cur = cur->next;
    }

    // 중복 or 겹침 검사는 간단하게: 같은 seq면 무시
    if (cur && cur->seq == seq)
        return;

    segment_t *node = malloc (sizeof (segment_t));
    node->seq = seq;
    node->payload_len = payload_len;
    node->is_FIN = is_fin;
    memcpy (node->data, data, payload_len);
    node->next = cur;

    if (prev)
        prev->next = node;
    else
        receive_buffer->head = node;
}

/* Receive side flush */
static void
flush_in_order_segments_from_receive_buffer (mysocket_t sd, context_t *ctx)
{
    /* suppose that buffer is sorted and non-overlapped */
    while (ctx->receive_buffer.head
           && ctx->receive_buffer.head->seq == ctx->receive_next)
    {
        segment_t *node = ctx->receive_buffer.head;

        if (node->payload_len > 0)
        {
            stcp_app_send (sd, node->data, node->payload_len);
            ctx->receive_next += node->payload_len;
        }

        if (node->is_FIN)
        {
            stcp_fin_received (sd);
            ctx->receive_next += 1;

            ctx->connection_state
                = transition_state (ctx->connection_state, EVENT_RECV_FIN);
            ctx->is_FIN_RECEIVED = TRUE;
            ctx->receive_buffer.head = node->next;
            free (node);
            return;
        }
        ctx->receive_buffer.head = node->next;
        free (node);
    }
}

/* Send side buffer enqueue */
static void
enqueue_send_buffer (segment_list_t *send_buffer, tcp_seq seq,
                     const uint8_t *data, size_t payload_len, bool_t is_fin)
{
    segment_t *node = malloc (sizeof (segment_t));
    node->seq = seq;
    node->payload_len = payload_len;
    node->is_FIN = is_fin;
    memcpy (node->data, data, payload_len);

    node->next = NULL;
    if (!send_buffer->head)
    {
        send_buffer->head = node;
        return;
    }
    segment_t *cur = send_buffer->head;
    while (cur->next)
        cur = cur->next;
    cur->next = node;
}

/* Send side buffer dequeue */
static void
dequeue_send_buffer (segment_list_t *send_buffer, tcp_seq ack)
{
    while (send_buffer->head
           && send_buffer->head->seq + send_buffer->head->payload_len
                      + (send_buffer->head->is_FIN ? 1 : 0)
                  <= ack)
    {
        segment_t *node = send_buffer->head;
        send_buffer->head = node->next;
        free (node);
    }
}
static void
resend_send_buffer (mysocket_t sd, segment_list_t *send_buffer,
                    tcp_seq send_base, tcp_seq receive_next)
{

    segment_t *cur = send_buffer->head;
    while (cur)
    {

        STCPHeader hdr;
        build_header (&hdr, cur->seq, receive_next,
                      TH_ACK | (cur->is_FIN ? TH_FIN : 0));
        stcp_network_send (sd, &hdr, sizeof (STCPHeader), cur->data,
                           cur->payload_len, NULL);
        cur = cur->next;
    }
}

/* utility function impl */

/* generate initial sequence number for an STCP connection */
static void
generate_initial_seq_num (context_t *ctx)
{
    assert (ctx);
    ctx->initial_sequence_num = 1;
}

static void
update_rto (context_t *ctx, bool_t is_timeout)
{
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
                devRTT = (1 - β) * devRTT + β * |sampleRTT - estRTT|
                # devRTT should come first
                estRTT = (1 - α) * estRTT + α * sampleRTT

            RTO = estRTT + 4 * devRTT

        retransmitted = false
*/

    assert (ctx);

    if (is_timeout)
    {
        ctx->rto *= 2;
        return;
    }

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

        ctx->dev_rtt
            = (1 - beta) * ctx->dev_rtt + beta * fabs (sample - ctx->est_rtt);
        ctx->est_rtt = (1 - alpha) * ctx->est_rtt + alpha * sample;
    }

    ctx->rto = ctx->est_rtt + 4 * ctx->dev_rtt;

    if (ctx->rto < MIN_RTO)
        ctx->rto = MIN_RTO;
    if (ctx->rto > MAX_RTO)
        ctx->rto = MAX_RTO;
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
    if (ctx->timeout.tv_nsec >= 1e9) // defensive purpose
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
is_valid_seg_len_and_flags (const STCPHeader *hdr, ssize_t len,
                            uint8_t expected_flags)
{
    if (len < (ssize_t)sizeof (STCPHeader))
        return FALSE;
    if ((hdr->th_flags & expected_flags) != expected_flags)
        return FALSE;
    return TRUE;
}

static stcp_state_t
transition_state (stcp_state_t state, stcp_event_t event)
{
    switch (state)
    {
    case CSTATE_ESTABLISHED:
        switch (event)
        {
        case EVENT_SEND_FIN:
            return CSTATE_FIN_WAIT_1;
        case EVENT_RECV_FIN:
            return CSTATE_CLOSE_WAIT;
        default:
            return state;
        }

    case CSTATE_FIN_WAIT_1:
        switch (event)
        {
        case EVENT_RECV_ACK:
            return CSTATE_FIN_WAIT_2;
        case EVENT_RECV_FIN:
            return CSTATE_CLOSING;
        default:
            return state;
        }

    case CSTATE_FIN_WAIT_2:
        switch (event)
        {
        case EVENT_RECV_FIN:
            return CSTATE_CLOSED;
        default:
            return state;
        }

    case CSTATE_CLOSING:
        switch (event)
        {
        case EVENT_RECV_ACK:
            return CSTATE_CLOSED;
        default:
            return state;
        }

    case CSTATE_CLOSE_WAIT:
        switch (event)
        {
        case EVENT_SEND_FIN:
            return CSTATE_LAST_ACK;
        default:
            return state;
        }

    case CSTATE_LAST_ACK:
        switch (event)
        {
        case EVENT_RECV_ACK:
            return CSTATE_CLOSED;
        default:
            return state;
        }

    default:
        return state;
    }
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

    segment_list_init (&ctx->send_buffer);
    segment_list_init (&ctx->receive_buffer);
    ctx->next_seq_num = ctx->initial_sequence_num;
    ctx->send_base = ctx->initial_sequence_num;
    ctx->rto = 1.0;
    ctx->est_rtt = -1.0;
    ctx->dev_rtt = 0.0;
    ctx->done = FALSE;

    int handshake_result = is_active ? do_active_handshake (sd, ctx)
                                     : do_passive_handshake (sd, ctx);

    if (handshake_result < 0)
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

    segment_list_clear (&ctx->send_buffer);
    segment_list_clear (&ctx->receive_buffer);
    free (ctx);
}

/* Handshake logic implementation */
static int
do_active_handshake (mysocket_t sd, context_t *ctx)
{
    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];

    /* build and send SYN */
    STCPHeader syn;
    build_header (&syn, ctx->next_seq_num, 0, TH_SYN);

    ctx->retransmission_count = 0;
    while (ctx->retransmission_count <= MAX_RETRANSMISSION_COUNT)
    {
        stcp_network_send (sd, &syn, sizeof (STCPHeader), NULL);

        if (ctx->retransmission_count == 0)
            ctx->next_seq_num++; // SYN sent, increment seq_num

        log_time_and_calculate_timeout (ctx);

        unsigned int event
            = stcp_wait_for_event (sd, NETWORK_DATA, &ctx->timeout);

        /* timeout case */
        if (event == TIMEOUT)
        {
            update_rto (ctx, TRUE);
            ctx->retransmission_count++;
            continue;
        }

        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        STCPHeader *synack = (STCPHeader *)buf;

        if (!is_valid_seg_len_and_flags (synack, n, TH_SYN | TH_ACK))
        {
            continue;
        }

        /* Check Ack num == base + 1 */
        tcp_seq peer_ack = ntohl (synack->th_ack);

        if (peer_ack != ctx->send_base + 1)
        {
            continue;
        }

        /* Connection established  */
        if (ctx->retransmission_count == 0)
            update_rto (ctx, FALSE);

        /* update send_base, peer's seq number, receive_next, and
         * retransmission_count */
        ctx->send_base = peer_ack;
        ctx->peer_initial_seq = ntohl (synack->th_seq);
        ctx->receive_next = ctx->peer_initial_seq + 1;
        ctx->retransmission_count = 0;
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
    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];

    /* receive SYN */
    STCPHeader *syn;
    while (1)
    {
        /* wait without timeout */
        stcp_wait_for_event (sd, NETWORK_DATA, NULL);

        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        syn = (STCPHeader *)buf;

        if (is_valid_seg_len_and_flags (syn, n, TH_SYN))
        {
            ctx->peer_initial_seq = ntohl (syn->th_seq);
            ctx->receive_next = ctx->peer_initial_seq + 1;
            break;
        }
    }

    /* build SYNACK segment */
    STCPHeader synack;
    build_header (&synack, ctx->next_seq_num, ctx->receive_next,
                  TH_SYN | TH_ACK);

    ctx->retransmission_count = 0;
    while (ctx->retransmission_count < 6)
    {
        /* send SYNACK */
        stcp_network_send (sd, &synack, sizeof (STCPHeader), NULL);
        log_time_and_calculate_timeout (ctx);

        ctx->next_seq_num
            = ctx->send_base + 1; // SYNACK sent, increase 1 from base

        /* wait for ACK */
        unsigned int event
            = stcp_wait_for_event (sd, NETWORK_DATA, &ctx->timeout);

        /* timeout case */
        if (!(event & NETWORK_DATA))
        {

            update_rto (ctx, TRUE);
            ctx->retransmission_count++;
            continue;
        }

        /* receive ack */
        ssize_t n = stcp_network_recv (sd, buf, sizeof (buf));

        STCPHeader *ack = (STCPHeader *)buf;

        if (!is_valid_seg_len_and_flags (ack, n, TH_ACK))
        {
            continue;
        }

        /* Check Ack num == base */
        tcp_seq peer_ack = ntohl (ack->th_ack);

        if (peer_ack != ctx->send_base + 1)
        {
            continue;
        }

        if (ctx->retransmission_count == 0)
            update_rto (ctx, FALSE);

        /* update send_base, peer's seq number, receive_next, and
         * retransmission_count */
        ctx->send_base = peer_ack;
        ctx->peer_initial_seq = ntohl (ack->th_seq);
        int header_len = ack->th_off * 4;
        int payload_len = n - header_len;
        if (payload_len > 0)
        {
            stcp_app_send (sd, buf + header_len, payload_len);
        }
        ctx->receive_next = ctx->peer_initial_seq + payload_len;
        ctx->retransmission_count = 0;
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

    // ctx->retransmission_count = 0; is guranteed by the handshake logic
    while (!ctx->done)
    {
        unsigned int event;

        // timeout if there's any in-flight segment
        bool_t waiting_for_ack = (ctx->send_base != ctx->next_seq_num);

        unsigned int wait_flag = NETWORK_DATA;
        int send_win_remain
            = (ctx->send_base + DEFAULT_WIN_SIZE) - ctx->next_seq_num;

        // wait for send event only if there's space in the send window
        if (send_win_remain > 0)
        {
            wait_flag |= (APP_DATA | APP_CLOSE_REQUESTED);
        }

        event = stcp_wait_for_event (sd, wait_flag,
                                     waiting_for_ack ? &ctx->timeout : NULL);

        if (event & APP_DATA)
        {
            handle_app_data (sd, ctx);
        }

        if (event & NETWORK_DATA)

        {
            handle_network_data (sd, ctx);
        }

        if (event & APP_CLOSE_REQUESTED)
        {
            handle_app_close (sd, ctx);
        }

        if (event == TIMEOUT)
        {
            handle_timeout (sd, ctx);
        }

        ctx->done = (ctx->connection_state == CSTATE_CLOSED);
    }
}

/* Control loop handlers implementation */
static void
handle_app_data (mysocket_t sd, context_t *ctx)
{
    if (ctx->is_FIN_SENT) // after CLOSE requested from app, ignore APP_DATA
    {
        return;
    }

    int win_remain = (ctx->send_base + DEFAULT_WIN_SIZE) - ctx->next_seq_num;
    size_t len_to_read = (win_remain < STCP_MSS) ? win_remain : STCP_MSS;

    if (len_to_read == 0) // already checked in control_loop,,
        return;

    uint8_t payload[STCP_MSS];
    size_t payload_len = stcp_app_recv (sd, payload, len_to_read);

    if (payload_len == 0)
        return;

    /* build and send segment */
    STCPHeader hdr;
    build_header (&hdr, ctx->next_seq_num, ctx->receive_next, TH_ACK);
    stcp_network_send (sd, &hdr, sizeof (STCPHeader), payload, payload_len,
                       NULL);

    /* buffer to send_window */
    enqueue_send_buffer (&ctx->send_buffer, ctx->next_seq_num, payload,
                         payload_len, FALSE);

    /* if it is first in-flight segment, start timer */
    if (ctx->send_base == ctx->next_seq_num)
        log_time_and_calculate_timeout (ctx);

    /* update next_seq_num */
    ctx->next_seq_num += payload_len;
}

static void
handle_network_data (mysocket_t sd, context_t *ctx)
{
    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];
    size_t n = stcp_network_recv (sd, buf, sizeof (buf));

    if (!is_valid_seg_len_and_flags ((STCPHeader *)buf, n, 0))
        return;

    /* parse header and payload */
    STCPHeader *hdr = (STCPHeader *)buf;
    int header_len = hdr->th_off * 4;
    int payload_len = n - header_len;
    uint8_t *payload = buf + header_len;

    tcp_seq seq = ntohl (hdr->th_seq);
    tcp_seq ack = ntohl (hdr->th_ack);

    uint8_t flags = hdr->th_flags;

    int is_fin = (flags & TH_FIN) ? 1 : 0;

    /* ACK handler */
    if (flags & TH_ACK)
    {
        handle_ack_from_peer (sd, ctx, ack);
    }

    /* Payload, FIN handler */
    if (payload_len > 0 || is_fin)
    {
        handle_incoming_segment (sd, ctx, seq, payload, payload_len, is_fin);
    }
}

static void
handle_app_close (mysocket_t sd, context_t *ctx)
{
    if (ctx->is_FIN_SENT) // after CLOSE requested from app, ignore APP_CLOSE
    {
        return;
    }

    int win_remain = (ctx->send_base + DEFAULT_WIN_SIZE) - ctx->next_seq_num;
    if (win_remain > 0)
    {
        STCPHeader hdr;
        build_header (&hdr, ctx->next_seq_num, ctx->receive_next,
                      TH_ACK | TH_FIN);
        stcp_network_send (sd, &hdr, sizeof (STCPHeader), NULL);

        enqueue_send_buffer (&ctx->send_buffer, ctx->next_seq_num, NULL, 0,
                             TRUE);

        if (ctx->send_base == ctx->next_seq_num)
            log_time_and_calculate_timeout (ctx);

        ctx->next_seq_num += 1;
    }
    else
    {
        /*
            whenever a packet is acked and send buffer is flushed
            and if is_APP_CLOSE_QUEUED is TRUE then FIN will be sent and
            buffered
        */
        ctx->is_APP_CLOSE_QUEUED = TRUE;
    }
    // state transition, set flag
    ctx->is_FIN_SENT = TRUE;
    ctx->connection_state
        = transition_state (ctx->connection_state, EVENT_SEND_FIN);
}

static void
handle_timeout (mysocket_t sd, context_t *ctx)
{
    /* if already retransmitted 5 times, then set state CLOSED and set errno,
       then return
    */
    if (ctx->retransmission_count == 5)
    {
        ctx->connection_state = CSTATE_CLOSED;
        errno = ECONNABORTED;
        return;
    }

    // Go-Back-N: send all buffered in-flight packets
    resend_send_buffer (sd, &ctx->send_buffer, ctx->send_base,
                        ctx->receive_next);

    /*
        timeout happened, update rto
        after update rto, set new timeout
     */
    update_rto (ctx, TRUE);
    log_time_and_calculate_timeout (ctx);

    ctx->retransmission_count++;
}

static void
handle_ack_from_peer (mysocket_t sd, context_t *ctx, tcp_seq ack)
{
    if (ack <= ctx->send_base || ack > ctx->next_seq_num)
        return;

    if (ctx->retransmission_count == 0)
        update_rto (ctx, FALSE);

    dequeue_send_buffer (&ctx->send_buffer, ack);
    ctx->send_base = ack;
    ctx->retransmission_count = 0;

    if (ctx->send_base != ctx->next_seq_num)
        log_time_and_calculate_timeout (ctx);

    /*
        whenever a packet is acked and send buffer is flushed
        and if is_APP_CLOSE_QUEUED is TRUE then FIN will be sent and
        buffered
    */
    if (ctx->is_APP_CLOSE_QUEUED)
    {
        int win_remain
            = (ctx->send_base + DEFAULT_WIN_SIZE) - ctx->next_seq_num;
        assert (win_remain > 0); // after ack, window size should be positive

        STCPHeader hdr;
        build_header (&hdr, ctx->next_seq_num, ctx->receive_next,
                      TH_ACK | TH_FIN);
        stcp_network_send (sd, &hdr, sizeof (STCPHeader), NULL);

        enqueue_send_buffer (&ctx->send_buffer, ctx->next_seq_num, NULL, 0,
                             TRUE);

        if (ctx->send_base == ctx->next_seq_num)
            log_time_and_calculate_timeout (ctx);

        ctx->next_seq_num += 1;
        ctx->is_APP_CLOSE_QUEUED = FALSE;
    }

    /* If there's no in-flight segment after FIN sent,
       it means that our FIN is acked by peer.
    */
    if (ctx->is_FIN_SENT && ctx->send_base == ctx->next_seq_num)
    {
        ctx->connection_state
            = transition_state (ctx->connection_state, EVENT_RECV_ACK);
    }
}

static void
handle_incoming_segment (mysocket_t sd, context_t *ctx, tcp_seq seq,
                         const uint8_t *payload, size_t payload_len,
                         bool_t is_fin)
{
    if (ctx->is_FIN_RECEIVED) // After acked peer's FIN, only care about ACK
                              // from peer

    {
        return;
    }

    tcp_seq seg_start = seq;
    tcp_seq seg_end = seq + payload_len + (is_fin ? 1 : 0);

    tcp_seq win_start = ctx->receive_next;
    tcp_seq win_end = win_start + DEFAULT_WIN_SIZE;

    /* too old data, send ACK for receive_next */
    if (seg_end <= win_start)
    {
        STCPHeader ack;
        build_header (&ack, ctx->next_seq_num, ctx->receive_next, TH_ACK);
        stcp_network_send (sd, &ack, sizeof (STCPHeader), NULL);

        return;
    }

    /* too new data, simply drop */
    if (seg_start >= win_end)
        return;

    /* Handling segments that partially overlap with the receive window */
    /* Drop already received data from front-side */
    if (seg_start < win_start)
    {
        size_t cut = win_start - seg_start;
        if (cut < payload_len)
        {
            payload += cut;
            payload_len -= cut;
        }
        else
        {
            payload_len = 0;
        }
    }

    /* Drop beyond the receive window from back-side */
    if (seg_end > win_end)
    {
        size_t cut = seg_end - win_end;
        if (is_fin)
        {
            is_fin = 0; // Drop FIN which is last byte
            cut--;
        }
        payload_len -= cut;
    }

    insert_segment_to_receive_buffer (&ctx->receive_buffer, seq, payload,
                                      payload_len, is_fin);

    // flush in-order data to app and update receive_next
    flush_in_order_segments_from_receive_buffer (sd, ctx);

    // send ACK
    STCPHeader ack;
    build_header (&ack, ctx->next_seq_num, ctx->receive_next, TH_ACK);
    stcp_network_send (sd, &ack, sizeof (STCPHeader), NULL);
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
