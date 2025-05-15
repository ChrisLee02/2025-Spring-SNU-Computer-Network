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
#define MAX_SEGMENTS (DEFAULT_WIN_SIZE / STCP_MSS + 1)

enum
{
    CSTATE_ESTABLISHED,
    CLOSE_REQUESTED,
    CSTATE_FIN_SENT,
    CSTATE_CLOSE_WAIT
}; /* obviously you should have more states */

typedef struct
{
    tcp_seq seq;            // 시작 시퀀스 번호
    uint8_t data[STCP_MSS]; // 데이터 내용
    size_t len;             // 유효한 payload 길이
    bool_t is_FIN;
} segment_buf_t;
// segment_buf_t buffer[MAX_SEGMENTS]; 에 대해서,
// send_buffer[0] ~ base ~ base + STCP_MSS
// send_buffer[1] ~ buffer[0] + buffer[0] + STCP_MSS
// send_buffer[2] ~ buffer[1] + buffer[1] + STCP_MSS
// send_buffer[3] ~ buffer[2] + buffer[2] + STCP_MSS
// send_buffer[4] ~ buffer[3] + buffer[3] + STCP_MSS
// send_buffer[5] ~ buffer[4] + buffer[4] + STCP_MSS

// 까지 하면 3072, 576 에대해서 대응 가능하다.

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
    segment_buf_t send_buffer[MAX_SEGMENTS]; // 수신 버퍼

    /* recv window */
    tcp_seq receive_next; // 다음으로 기대하는 시퀀스 번호 (in-order 수신 기준)
    segment_buf_t recv_buffer[MAX_SEGMENTS]; // 수신 버퍼

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

/* Handshake logic */
static int do_active_handshake (mysocket_t sd, context_t *ctx);
static int do_passive_handshake (mysocket_t sd, context_t *ctx);

/* Control loop */
static void control_loop (mysocket_t sd, context_t *ctx);

/* Control loop handlers */
static void handle_app_data (mysocket_t sd, context_t *ctx);
static void handle_network_data (mysocket_t sd, context_t *ctx);
static void handle_app_close (mysocket_t sd, context_t *ctx);
static void handle_timeout (mysocket_t sd, context_t *ctx);

static void handle_ack (context_t *ctx, tcp_seq ack);
static void handle_data (mysocket_t sd, context_t *ctx, tcp_seq seq,
                         const uint8_t *payload, size_t len);
static void handle_fin (context_t *ctx);

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

/* Handshake logic implementation */
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
        // todo: 사실 여기는 network_recv가 세그먼트 단위로 받고 잘린 부분은
        // 버리기 때문에, 굳이 페이로드 버퍼를 둘 필욘 없었음,,
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
        ctx->retransmission_count = 0;
        tcp_seq peer_seq_start = ntohl (ack->th_seq);
        int header_len = ack->th_off * 4;
        int payload_len = n - header_len;
        if (payload_len > 0)
        {
            stcp_app_send (sd, buf + header_len, payload_len);
        }
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
    // ctx->retransmission_count = 0; is guranteed by the handshake logic
    while (!ctx->done)
    {
        unsigned int event;

        bool_t waiting_for_ack = (ctx->send_base != ctx->next_seq_num);

        event = stcp_wait_for_event (sd, ANY_EVENT,
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

        if (event & TIMEOUT)
        {
            handle_timeout (sd, ctx);
        }
    }
}

/* Control loop handlers implementation */
static void
handle_app_data (mysocket_t sd, context_t *ctx)
{
    int win_remain = (ctx->send_base + DEFAULT_WIN_SIZE) - ctx->next_seq_num;
    size_t len_to_read = (win_remain < STCP_MSS) ? win_remain : STCP_MSS;

    if (len_to_read == 0)
        return;

    uint8_t payload[STCP_MSS];
    size_t len = stcp_app_recv (sd, payload, len_to_read);

    if (len == 0)
        return;

    STCPHeader hdr;
    build_header (&hdr, ctx->next_seq_num, ctx->receive_next, TH_ACK);
    stcp_network_send (sd, &hdr, sizeof (STCPHeader), payload, len);

    if (ctx->send_base == ctx->next_seq_num)
        log_time_and_calculate_timeout (
            ctx); // 버퍼가 빈 상태에서 전송: 타이머 시작

    // buffer to send_window
    int index = (ctx->next_seq_num - ctx->send_base) / STCP_MSS;
    assert (index >= 0 && index < MAX_SEGMENTS);
    segment_buf_t *slot = &ctx->send_buffer[index];
    slot->seq = ctx->next_seq_num;
    slot->len = len;
    memcpy (slot->data, payload, len);

    // update next_seq_num
    ctx->next_seq_num += len;
}

static void
handle_network_data (mysocket_t sd, context_t *ctx)
{
    // TODO: receive segment from peer
    //       if ACK: update send_base + update timer, rto,
    //       init retransmission_count to 0
    //       if DATA: push to app, send ACK back
    //       if FIN: send ACK back, update state to CLOSE_WAIT

    uint8_t buf[sizeof (STCPHeader) + STCP_MSS];
    size_t n = stcp_network_recv (sd, buf, sizeof (buf));

    if (n < (size_t)sizeof (STCPHeader))
        return; // 잘못된 segment, 무시

    STCPHeader *hdr = (STCPHeader *)buf;
    int header_len = hdr->th_off * 4;
    int payload_len = n - header_len;
    uint8_t *payload = buf + header_len;

    tcp_seq seq = ntohl (hdr->th_seq);
    tcp_seq ack = ntohl (hdr->th_ack);

    uint8_t flags = hdr->th_flags;

    // ----- FIN 처리 -----
    if (flags & TH_FIN)
    {

        handle_fin (ctx);
    }

    // ----- ACK 처리 -----
    if (flags & TH_ACK)
    {
        handle_ack (ctx, ack);
    }

    // ----- 데이터 수신 처리 -----
    if (payload_len > 0)
    {
        handle_data (sd, ctx, seq, payload, payload_len)
        // TODO: in-order인지 확인 후, stcp_app_send() + ACK 응답
    }
}

static void
handle_app_close (mysocket_t sd, context_t *ctx)
{
    // TODO: send FIN
    //       update state to FIN_SENT
    //       wait for ACK or FIN
}

static void
handle_timeout (mysocket_t sd, context_t *ctx)
{
    // TODO: check retransmission_count
    //       if exceeds 6 → ctx->done = TRUE
    //       else resend unacked segment, double RTO
}

/*
 * handle_ack
 * -----------------------------
 * - 유효한 ACK 번호 수신 시 send_base ~ ack 사이 buffer 제거
 * - 이후 in-flight segment들을 버퍼 시작 위치로 shift
 * - 최초 전송에 대한 ACK면 RTO 갱신
 * - 타이머는 남은 데이터가 있을 때 재시작
 */
static void
handle_ack (context_t *ctx, tcp_seq ack)
{
    if (ack <= ctx->send_base || ack > ctx->next_seq_num)
        return;

    if (ctx->retransmission_count == 0)
        update_rto (ctx);

    int acked_bytes = ack - ctx->send_base;
    int seg_count = (acked_bytes + STCP_MSS - 1) / STCP_MSS;
    int total_count
        = (ctx->next_seq_num - ctx->send_base + STCP_MSS - 1) / STCP_MSS;

    // shift 후 남을 개수
    int remain = total_count - seg_count;

    for (int i = 0; i < remain; i++)
    {
        ctx->send_buffer[i] = ctx->send_buffer[i + seg_count];
    }

    // 뒷부분 무효화
    for (int i = remain; i < MAX_SEGMENTS; i++)
    {
        ctx->send_buffer[i].len = 0;
    }

    ctx->send_base = ack;

    if (ctx->send_base != ctx->next_seq_num)
        log_time_and_calculate_timeout (ctx);

    ctx->retransmission_count = 0;
}

static void
handle_data (mysocket_t sd, context_t *ctx, tcp_seq seq,
             const uint8_t *payload, size_t len)
{
    int index = (seq - ctx->peer_initial_seq) / STCP_MSS;

    if (index < 0 || index >= MAX_SEGMENTS + 2)
        return;

    // 수신 버퍼에 저장
    segment_buf_t *slot = &ctx->recv_buffer[index];
    if (slot->len == 0)
    { // 중복 방지
        slot->seq = seq;
        slot->len = len;
        memcpy (slot->data, payload, len);
    }

    // in-order merge
    while (1)
    {
        int rel_idx = (ctx->receive_next - ctx->peer_initial_seq) / STCP_MSS;

        if (rel_idx < 0 || rel_idx >= MAX_SEGMENTS + 2)
            break;

        segment_buf_t *cur = &ctx->recv_buffer[rel_idx];
        if (cur->len == 0 || cur->seq != ctx->receive_next)
            break;

        // 앱으로 전달
        stcp_app_send (sd, cur->data, cur->len);
        ctx->receive_next += cur->len;
        cur->len = 0;
    }

    // FIN 수신 후, 앱에게 EOF 통지
    if (ctx->connection_state == CSTATE_CLOSE_WAIT)
    {
        stcp_app_send (sd, NULL, 0);
    }
}

static void
handle_fin (context_t *ctx)
{
    if (ctx->connection_state != CSTATE_ESTABLISHED)
        return;

    ctx->receive_next += 1;
    ctx->connection_state = CSTATE_CLOSE_WAIT;
    // 종료는 app_close 핸들러에서 처리
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
