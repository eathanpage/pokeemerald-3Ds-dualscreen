/*
 * Connection to the online relay server, for the emulated Wireless Adapter
 * (3ds_rfu_adapter.c). The same protocol as the PC port's port/online.c, so
 * the two meet in the same rooms: see server/relay.py in the PC repository.
 *
 * A thread of its own keeps the connection: it joins the room the player
 * asked for (or has the server make one), reconnects when the connection
 * drops and queues what arrives for the game thread. Everything that goes out
 * is queued too and written by that thread, which owns the socket and the TLS
 * session (mbedTLS is not safe with a read and a write at once).
 *
 * The connection is plain TCP (online_server=host:port) or a WebSocket
 * (wss://host or ws://host:port), for a server behind a proxy or tunnel such
 * as Cloudflare's. Frames: u16 length (type + payload, little endian), u8
 * type, payload; over a WebSocket they are carried in binary messages, read
 * and written as one stream.
 *
 * The server's certificate is not checked: the console has no store of
 * authorities to check it against, and nothing private goes over the link
 * (no accounts; the relay sees what it relays anyway).
 */
#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include "3ds_log.h"
#include "3ds_online.h"
/* The room states and kinds the game's scripts see (the decomp tree). */
#include "../../include/constants/online.h"

/* 2: link battles use the adapter's direct battle channel, which version 1
 * games don't know: the server keeps versions apart. */
#define PROTOCOL_VERSION 2
#define RECONNECT_MS 3000
#define CONNECT_TIMEOUT_MS 10000
#define MAX_PAYLOAD 1024
#define EVENT_QUEUE 512
#define OUT_QUEUE 16384
#define WS_RX_SIZE 8192
#define STREAM_SIZE (2 * (3 + MAX_PAYLOAD))
#define SOC_BUFFER_SIZE 0x100000
#define THREAD_STACK (64 * 1024)
#define IDLE_POLL_MS 4

enum
{
    C_HELLO = 0x01,
    C_ADVERT = 0x02,
    C_SEND = 0x03,
    S_WELCOME = 0x81,
    S_ADVERT = 0x82,
    S_LEFT = 0x83,
    S_RECV = 0x84,
    S_ERROR = 0x85,
};

/* A connection to the server: only the thread touches it. */
struct Conn
{
    int fd;
    bool tls;
    bool ws;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    u8 wsRx[WS_RX_SIZE];       /* received WebSocket bytes not yet parsed */
    u32 wsRxLen;
    u8 stream[STREAM_SIZE];    /* relay bytes not yet parsed */
    u32 streamLen;
};

/* On the heap, set up before the game starts: a soft reset rewinds the
 * game's statics, not these, but the state is the thread's anyway. */
struct Online
{
    char host[128];
    char port[16];
    char path[128];
    bool webSocket;            /* wss://... or ws://... */
    bool secure;               /* wss */
    char room[16];             /* requested, then the one we're in (kept for reconnects) */

    LightLock lock;            /* everything below */
    LightEvent wake;           /* wakes the thread to (re)connect now */
    Thread thread;
    bool quit;
    bool dropConn;             /* end the current connection */
    u8 joinMode;               /* ONLINE_JOIN_*, for the next hello */
    u8 kind;                   /* ONLINE_KIND_* of the room */
    u8 wrongKind;              /* after ONLINE_ROOM_WRONG_KIND: the room's kind */
    u8 roomState;              /* ONLINE_ROOM_* */
    bool paused;               /* after the server turned us away, until Online_JoinRoom */
    bool rejoin;               /* Online_JoinRoom: say hello again right away */
    bool fixedRoom;            /* given at startup (tests): stay in it */
    bool connected;
    u16 myId;
    u32 generation;            /* +1 per (re)connection */
    char status[96];
    struct OnlinePeer peers[ONLINE_MAX_PEERS];
    bool haveAdvert;
    u8 advert[2 + ONLINE_ADVERT_SIZE];
    struct OnlineEvent events[EVENT_QUEUE];
    u32 eventHead, eventTail;
    u8 out[OUT_QUEUE];         /* frames for the thread to write */
    u32 outLen;
};

static struct Online *sOnline;
static void *sSocBuffer;
static bool sSocReady, sSslcReady;

uint32_t Online_Milliseconds(void)
{
    return (uint32_t)osGetTime();
}

static void SetStatus(const char *text)
{
    LightLock_Lock(&sOnline->lock);
    snprintf(sOnline->status, sizeof(sOnline->status), "%s", text);
    LightLock_Unlock(&sOnline->lock);
}

/* ------------------------------------------------------------------------- */
/* Network services, once, from the thread */

static bool StartServices(void)
{
    if (sSocReady)
        return true;
    if (sSocBuffer == NULL)
        sSocBuffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if (sSocBuffer == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "online: no memory for the socket service");
        return false;
    }
    Result rc = socInit(sSocBuffer, SOC_BUFFER_SIZE);
    if (R_FAILED(rc))
    {
        CtrLog_Write(CTR_LOG_ERROR, "online: socInit failed %08lx", (unsigned long)rc);
        return false;
    }
    sSocReady = true;
    /* The TLS random numbers come from the SSL service (or PS). */
    sSslcReady = R_SUCCEEDED(sslcInit(0));
    if (!sSslcReady)
        psInit();
    return true;
}

static int Entropy(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    if (sSslcReady && R_SUCCEEDED(sslcGenerateRandomData(out, len)))
        return 0;
    if (R_SUCCEEDED(PS_GenerateRandomBytes(out, len)))
        return 0;
    return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
}

/* ------------------------------------------------------------------------- */
/* Connections */

static bool ShouldStop(void)
{
    bool stop;

    LightLock_Lock(&sOnline->lock);
    stop = sOnline->quit || sOnline->dropConn;
    LightLock_Unlock(&sOnline->lock);
    return stop;
}

static int BioSend(void *ctx, const unsigned char *buf, size_t len)
{
    struct Conn *c = ctx;
    int n = send(c->fd, buf, len, 0);

    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len)
{
    struct Conn *c = ctx;
    int n = recv(c->fd, buf, len, 0);

    if (n >= 0)
        return n; /* 0: the other side closed */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* Waits up to `ms` for the socket to be readable (POLLIN) or writable. */
static void WaitSocket(struct Conn *c, short events, int ms)
{
    struct pollfd p = {.fd = c->fd, .events = events};
    poll(&p, 1, ms);
}

static void ConnClose(struct Conn *c)
{
    if (c == NULL)
        return;
    if (c->tls)
    {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
    }
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

/* Writes it all (waiting while the socket is full). */
static bool ConnWrite(struct Conn *c, const u8 *data, u32 len)
{
    u32 start = Online_Milliseconds();

    while (len)
    {
        int n = c->tls ? mbedtls_ssl_write(&c->ssl, data, len) : BioSend(c, data, len);

        if (n > 0)
        {
            data += n;
            len -= n;
            start = Online_Milliseconds();
            continue;
        }
        if (n != MBEDTLS_ERR_SSL_WANT_WRITE && n != MBEDTLS_ERR_SSL_WANT_READ)
            return false;
        if (Online_Milliseconds() - start > CONNECT_TIMEOUT_MS || ShouldStop())
            return false;
        WaitSocket(c, n == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 50);
    }
    return true;
}

/* What has come in: > 0 bytes, 0 nothing yet, < 0 the connection ended. */
static int ConnReadSome(struct Conn *c, u8 *buf, u32 max)
{
    int n = c->tls ? mbedtls_ssl_read(&c->ssl, buf, max) : BioRecv(c, buf, max);

    if (n > 0)
        return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return 0;
    return -1;
}

static bool ConnHasBuffered(struct Conn *c)
{
    return c->tls && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0;
}

static bool StartTls(struct Conn *c)
{
    u32 start = Online_Milliseconds();
    int rc;

    c->tls = true;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    if (mbedtls_ctr_drbg_seed(&c->drbg, Entropy, NULL, (const unsigned char *)"emerald3ds", 10) != 0
     || mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return false;
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE); /* see the top of the file */
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0
     || mbedtls_ssl_set_hostname(&c->ssl, sOnline->host) != 0) /* SNI: a proxy needs it */
        return false;
    mbedtls_ssl_set_bio(&c->ssl, c, BioSend, BioRecv, NULL);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0)
    {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            CtrLog_Write(CTR_LOG_ERROR, "online: TLS handshake failed -0x%04x", (unsigned)-rc);
            return false;
        }
        if (Online_Milliseconds() - start > CONNECT_TIMEOUT_MS || ShouldStop())
            return false;
        WaitSocket(c, rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 50);
    }
    return true;
}

/* The HTTP upgrade. What the server sends after its answer stays in wsRx. */
static bool StartWebSocket(struct Conn *c)
{
    char request[512];
    u8 key[16];
    char keyText[25];
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    u32 start = Online_Milliseconds();
    int i, j, len;

    c->ws = true;
    Entropy(NULL, key, sizeof(key));
    for (i = j = 0; i < 16; i += 3)
    {
        u32 v = key[i] << 16 | (i + 1 < 16 ? key[i + 1] << 8 : 0) | (i + 2 < 16 ? key[i + 2] : 0);
        keyText[j++] = b64[v >> 18 & 63];
        keyText[j++] = b64[v >> 12 & 63];
        keyText[j++] = i + 1 < 16 ? b64[v >> 6 & 63] : '=';
        keyText[j++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    keyText[j] = '\0';
    len = snprintf(request, sizeof(request),
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: Emerald3DS\r\n\r\n",
                   sOnline->path, sOnline->host, keyText);
    if (!ConnWrite(c, (const u8 *)request, len))
        return false;

    for (;;)
    {
        u8 *end;
        int n;

        c->wsRx[c->wsRxLen] = '\0';
        end = (u8 *)strstr((const char *)c->wsRx, "\r\n\r\n");
        if (end)
        {
            u32 headerLen = end + 4 - c->wsRx;

            if (strncmp((const char *)c->wsRx, "HTTP/1.1 101", 12) != 0)
            {
                CtrLog_Write(CTR_LOG_ERROR, "online: the server refused the WebSocket: %.40s",
                             (const char *)c->wsRx);
                return false;
            }
            memmove(c->wsRx, c->wsRx + headerLen, c->wsRxLen - headerLen);
            c->wsRxLen -= headerLen;
            return true;
        }
        if (c->wsRxLen >= 2048)
            return false;
        n = ConnReadSome(c, c->wsRx + c->wsRxLen, 2048 - c->wsRxLen);
        if (n < 0)
            return false;
        if (n > 0)
            c->wsRxLen += n;
        else if (!ConnHasBuffered(c))
        {
            if (Online_Milliseconds() - start > CONNECT_TIMEOUT_MS || ShouldStop())
                return false;
            WaitSocket(c, POLLIN, 50);
        }
    }
}

static struct Conn *ConnOpen(void)
{
    struct addrinfo hints, *res = NULL, *ai;
    struct Conn *c;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(sOnline->host, sOnline->port, &hints, &res) != 0 || res == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "online: can't look up %s", sOnline->host);
        return NULL;
    }
    for (ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return NULL;
    {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    c = calloc(1, sizeof(*c));
    if (c == NULL)
    {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    if ((sOnline->secure && !StartTls(c)) || (sOnline->webSocket && !StartWebSocket(c)))
    {
        ConnClose(c);
        return NULL;
    }
    return c;
}

/* Writes the queued frames (one WebSocket message for the lot). */
static bool FlushOut(struct Conn *c)
{
    static u8 buf[14 + OUT_QUEUE];
    u32 len, header = 0;

    LightLock_Lock(&sOnline->lock);
    len = sOnline->outLen;
    memcpy(buf + 14, sOnline->out, len);
    sOnline->outLen = 0;
    LightLock_Unlock(&sOnline->lock);
    if (len == 0)
        return true;
    if (c->ws)
    {
        u8 mask[4];
        u8 *frame;

        /* FIN + binary, masked as a client's must be. */
        Entropy(NULL, mask, sizeof(mask));
        header = len < 126 ? 6 : 8;
        frame = buf + 14 - header;
        frame[0] = 0x82;
        if (len < 126)
        {
            frame[1] = 0x80 | len;
        }
        else
        {
            frame[1] = 0x80 | 126;
            frame[2] = len >> 8;
            frame[3] = len;
        }
        memcpy(frame + header - 4, mask, 4);
        for (u32 i = 0; i < len; i++)
            buf[14 + i] ^= mask[i & 3];
    }
    return ConnWrite(c, buf + 14 - header, header + len);
}

/* A control answer (pong, close) straight from the thread. */
static bool SendWsControl(struct Conn *c, u8 opcode, const u8 *payload, u32 len)
{
    u8 frame[6 + 125];

    if (len > 125)
        len = 125;
    frame[0] = 0x80 | opcode;
    frame[1] = 0x80 | len;
    Entropy(NULL, frame + 2, 4);
    for (u32 i = 0; i < len; i++)
        frame[6 + i] = payload[i] ^ frame[2 + (i & 3)];
    return ConnWrite(c, frame, 6 + len);
}

static struct OnlinePeer *FindPeer(u16 id, bool create)
{
    struct OnlinePeer *empty = NULL;

    for (int i = 0; i < ONLINE_MAX_PEERS; i++)
    {
        if (sOnline->peers[i].valid && sOnline->peers[i].id == id)
            return &sOnline->peers[i];
        if (!sOnline->peers[i].valid && !empty)
            empty = &sOnline->peers[i];
    }
    if (create && empty)
    {
        memset(empty, 0, sizeof(*empty));
        empty->valid = 1;
        empty->id = id;
        return empty;
    }
    return NULL;
}

/* Called with the lock held. */
static void PushEvent(u8 type, u16 id, const u8 *data, u32 len)
{
    u32 next = (sOnline->eventTail + 1) % EVENT_QUEUE;
    struct OnlineEvent *ev;

    if (next == sOnline->eventHead)
        return; /* the game isn't reading (adapter off): drop */
    ev = &sOnline->events[sOnline->eventTail];
    ev->type = type;
    ev->id = id;
    ev->len = len < sizeof(ev->data) ? len : sizeof(ev->data);
    if (ev->len)
        memcpy(ev->data, data, ev->len);
    sOnline->eventTail = next;
}

/* Called with the lock held. */
static void QueueFrame(u8 type, const void *a, u32 alen, const void *b, u32 blen)
{
    u32 len = 1 + alen + blen;
    u8 *out;

    if (len > 1 + MAX_PAYLOAD || sOnline->outLen + 2 + len > OUT_QUEUE)
    {
        CtrLog_Write(CTR_LOG_ERROR, "online: send queue full, frame dropped");
        return;
    }
    out = sOnline->out + sOnline->outLen;
    out[0] = len;
    out[1] = len >> 8;
    out[2] = type;
    if (alen)
        memcpy(out + 3, a, alen);
    if (blen)
        memcpy(out + 3 + alen, b, blen);
    sOnline->outLen += 2 + len;
}

/* One frame from the server. */
static void HandleFrame(u8 type, const u8 *body, u32 len)
{
    char text[96];

    LightLock_Lock(&sOnline->lock);
    switch (type)
    {
    case S_WELCOME:
        if (len >= 3 && body[2] < sizeof(sOnline->room) && len >= 3u + body[2])
        {
            sOnline->myId = body[0] | (body[1] << 8);
            memcpy(sOnline->room, body + 3, body[2]);
            sOnline->room[body[2]] = '\0';
            sOnline->connected = true;
            sOnline->generation++;
            sOnline->roomState = ONLINE_ROOM_IN;
            /* Reconnecting later: back into the same room, even if it
             * emptied meanwhile. */
            sOnline->joinMode = ONLINE_JOIN_OR_CREATE;
            snprintf(sOnline->status, sizeof(sOnline->status), "Online room %s", sOnline->room);
            CtrLog_Write(CTR_LOG_GAME, "online: in room %s as %04x", sOnline->room, sOnline->myId);
            if (sOnline->haveAdvert)
                QueueFrame(C_ADVERT, sOnline->advert, sizeof(sOnline->advert), NULL, 0);
        }
        break;
    case S_ADVERT:
        if (len >= 2)
        {
            u16 id = body[0] | (body[1] << 8);
            struct OnlinePeer *peer = FindPeer(id, true);

            if (peer)
            {
                peer->hasAdvert = (len == 2 + 2 + ONLINE_ADVERT_SIZE);
                if (peer->hasAdvert)
                {
                    peer->hosting = body[2] & 1;
                    /* Only real hosting counts: a player who has only been
                     * searching mustn't look like a recent host. */
                    if (peer->hosting)
                        peer->lastHostingMs = Online_Milliseconds() | 1;
                    peer->nextSlot = body[3];
                    memcpy(peer->advert, body + 4, ONLINE_ADVERT_SIZE);
                }
                else
                {
                    peer->hosting = 0;
                }
            }
        }
        break;
    case S_LEFT:
        if (len >= 2)
        {
            u16 id = body[0] | (body[1] << 8);
            struct OnlinePeer *peer = FindPeer(id, false);

            if (peer)
                peer->valid = 0;
            PushEvent(ONLINE_EV_LEFT, id, NULL, 0);
        }
        break;
    case S_RECV:
        if (len >= 2)
            PushEvent(ONLINE_EV_RECV, body[0] | (body[1] << 8), body + 2, len - 2);
        break;
    case S_ERROR:
    {
        u32 skip = (len && body[0] == 5) ? 2 : 1; /* "wrong kind" carries the room's kind first */

        snprintf(text, sizeof(text), "Online: %.*s", (int)(len > skip ? len - skip : 0),
                 (const char *)body + skip);
        snprintf(sOnline->status, sizeof(sOnline->status), "%s", text);
        CtrLog_Write(CTR_LOG_GAME, "online: %s", text);
        switch (len ? body[0] : 0)
        {
        case 2:  sOnline->roomState = ONLINE_ROOM_FULL; break;
        case 3:  sOnline->roomState = ONLINE_ROOM_BAD_CODE; break;
        case 4:  sOnline->roomState = ONLINE_ROOM_NOT_FOUND; break;
        case 5:
            sOnline->roomState = ONLINE_ROOM_WRONG_KIND;
            sOnline->wrongKind = len > 1 ? body[1] : 0;
            break;
        case 6:  sOnline->roomState = ONLINE_ROOM_OTHER_VERSION; break;
        default: sOnline->roomState = ONLINE_ROOM_OLD_VERSION; break;
        }
        /* Out of rooms (the one we were in, if any, is left) until the
         * player tries another code. */
        sOnline->room[0] = '\0';
        sOnline->paused = true;
        break;
    }
    }
    LightLock_Unlock(&sOnline->lock);
}

/* Takes the relay frames out of the stream. FALSE: a malformed one. */
static bool ParseStream(struct Conn *c)
{
    u32 pos = 0;

    while (c->streamLen - pos >= 3)
    {
        u32 len = c->stream[pos] | (c->stream[pos + 1] << 8);

        if (len == 0 || len - 1 > MAX_PAYLOAD)
            return false;
        if (c->streamLen - pos < 2 + len)
            break;
        HandleFrame(c->stream[pos + 2], c->stream + pos + 3, len - 1);
        pos += 2 + len;
    }
    memmove(c->stream, c->stream + pos, c->streamLen - pos);
    c->streamLen -= pos;
    return true;
}

static bool AddToStream(struct Conn *c, const u8 *data, u32 len)
{
    while (len)
    {
        u32 n = sizeof(c->stream) - c->streamLen;

        /* The stream holds two of the largest frames: once full, there is
         * always a whole one to take out. */
        if (n == 0)
            return false;
        if (n > len)
            n = len;
        memcpy(c->stream + c->streamLen, data, n);
        c->streamLen += n;
        data += n;
        len -= n;
        if (!ParseStream(c))
            return false;
    }
    return true;
}

/* Takes the WebSocket frames out of wsRx. FALSE: the connection is to end. */
static bool ParseWebSocket(struct Conn *c)
{
    for (;;)
    {
        u8 *f = c->wsRx;
        u32 have = c->wsRxLen, header = 2, len;
        u8 opcode;

        if (have < 2)
            return true;
        opcode = f[0] & 0x0F;
        len = f[1] & 0x7F;
        if (len == 126)
        {
            if (have < 4)
                return true;
            len = f[2] << 8 | f[3];
            header = 4;
        }
        else if (len == 127)
        {
            if (have < 10)
                return true;
            if (f[2] | f[3] | f[4] | f[5] | f[6] | f[7])
                return false;
            len = (u32)f[8] << 8 | f[9];
            header = 10;
        }
        if (f[1] & 0x80)
            header += 4; /* the server shouldn't mask, but may */
        if (header + len > sizeof(c->wsRx))
            return false;
        if (have < header + len)
            return true;
        if (f[1] & 0x80)
        {
            const u8 *mask = f + header - 4;
            for (u32 i = 0; i < len; i++)
                f[header + i] ^= mask[i & 3];
        }
        switch (opcode)
        {
        case 0x0: /* continuation */
        case 0x2: /* binary */
            if (!AddToStream(c, f + header, len))
                return false;
            break;
        case 0x8: /* close: answer it and stop */
            SendWsControl(c, 0x8, f + header, len < 2 ? len : 2);
            return false;
        case 0x9: /* ping */
            if (!SendWsControl(c, 0xA, f + header, len))
                return false;
            break;
        default: /* pong, text: nothing to do */
            break;
        }
        memmove(c->wsRx, c->wsRx + header + len, have - header - len);
        c->wsRxLen -= header + len;
    }
}

/* One connection's lifetime: hello, then back and forth until it drops. */
static void RunConnection(struct Conn *c)
{
    u8 hello[5 + 16];
    u32 roomLen;

    LightLock_Lock(&sOnline->lock);
    sOnline->outLen = 0; /* anything queued was for the last connection */
    roomLen = strlen(sOnline->room);
    hello[0] = PROTOCOL_VERSION & 0xFF;
    hello[1] = PROTOCOL_VERSION >> 8;
    hello[2] = roomLen;
    memcpy(hello + 3, sOnline->room, roomLen);
    hello[3 + roomLen] = sOnline->joinMode;
    hello[4 + roomLen] = sOnline->kind;
    QueueFrame(C_HELLO, hello, 5 + roomLen, NULL, 0);
    LightLock_Unlock(&sOnline->lock);

    /* Anything that came with the WebSocket's answer. */
    if (c->ws && !ParseWebSocket(c))
        return;

    while (!ShouldStop())
    {
        u8 buf[2048];
        bool busy = false;
        int n;

        if (!FlushOut(c))
            break;
        for (;;)
        {
            if (c->ws)
                n = ConnReadSome(c, c->wsRx + c->wsRxLen, sizeof(c->wsRx) - c->wsRxLen);
            else
                n = ConnReadSome(c, buf, sizeof(buf));
            if (n <= 0)
                break;
            busy = true;
            if (c->ws)
            {
                c->wsRxLen += n;
                if (!ParseWebSocket(c))
                {
                    n = -1;
                    break;
                }
            }
            else if (!AddToStream(c, buf, n))
            {
                n = -1;
                break;
            }
        }
        if (n < 0)
            break;
        if (!busy && !ConnHasBuffered(c))
        {
            bool queued;

            LightLock_Lock(&sOnline->lock);
            queued = sOnline->outLen != 0;
            LightLock_Unlock(&sOnline->lock);
            if (!queued)
                WaitSocket(c, POLLIN, IDLE_POLL_MS);
        }
    }
}

static void OnlineThread(void *unused)
{
    (void)unused;
    for (;;)
    {
        struct Conn *c;
        bool paused, quit;

        LightLock_Lock(&sOnline->lock);
        paused = sOnline->paused;
        quit = sOnline->quit;
        sOnline->rejoin = false;
        sOnline->dropConn = false;
        LightLock_Unlock(&sOnline->lock);
        if (quit)
            break;
        if (paused)
        {
            LightEvent_Wait(&sOnline->wake);
            continue;
        }
        SetStatus("Online: connecting...");
        c = StartServices() ? ConnOpen() : NULL;
        if (c)
        {
            RunConnection(c);
            LightLock_Lock(&sOnline->lock);
            if (sOnline->roomState == ONLINE_ROOM_IN)
                sOnline->roomState = ONLINE_ROOM_CONNECTING;
            if (sOnline->connected)
            {
                /* Everyone in the room is gone as far as we know. */
                for (int i = 0; i < ONLINE_MAX_PEERS; i++)
                    if (sOnline->peers[i].valid)
                        PushEvent(ONLINE_EV_LEFT, sOnline->peers[i].id, NULL, 0);
                memset(sOnline->peers, 0, sizeof(sOnline->peers));
            }
            sOnline->connected = false;
            if (sOnline->roomState == ONLINE_ROOM_NONE)
                sOnline->status[0] = '\0'; /* left the room */
            else if (strncmp(sOnline->status, "Online: ", 8) != 0 || !strcmp(sOnline->status, "Online: connecting..."))
                snprintf(sOnline->status, sizeof(sOnline->status), "Online: connection lost");
            LightLock_Unlock(&sOnline->lock);
            ConnClose(c);
        }
        else
        {
            LightLock_Lock(&sOnline->lock);
            snprintf(sOnline->status, sizeof(sOnline->status), "Online: can't reach the server");
            if (sOnline->roomState == ONLINE_ROOM_CONNECTING)
                sOnline->roomState = ONLINE_ROOM_NO_SERVER;
            LightLock_Unlock(&sOnline->lock);
        }
        /* Wait before trying again, unless a new room was asked for. */
        LightLock_Lock(&sOnline->lock);
        paused = sOnline->rejoin || sOnline->quit;
        LightLock_Unlock(&sOnline->lock);
        if (!paused)
            LightEvent_WaitTimeout(&sOnline->wake, (s64)RECONNECT_MS * 1000000);
    }
}

/* ------------------------------------------------------------------------- */
/* The game's side */

/* Upper-case letters and digits only, at most 8. */
static void CopyRoomCode(char *dest, const char *room)
{
    int i, j;

    for (i = j = 0; room && room[i] && j < 8; i++)
    {
        char ch = room[i];

        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            dest[j++] = ch;
    }
    dest[j] = '\0';
}

/* Ends the current connection (the thread then makes a new one, or not). */
static void ShutdownConnection(void)
{
    LightLock_Lock(&sOnline->lock);
    sOnline->dropConn = true;
    LightLock_Unlock(&sOnline->lock);
}

void Online_JoinRoom(const char *room, uint8_t mode, uint8_t kind)
{
    if (!sOnline)
        return;
    LightLock_Lock(&sOnline->lock);
    CopyRoomCode(sOnline->room, room);
    sOnline->kind = kind;
    sOnline->joinMode = sOnline->room[0] ? mode : ONLINE_JOIN_OR_CREATE;
    sOnline->roomState = ONLINE_ROOM_CONNECTING;
    sOnline->paused = false;
    sOnline->rejoin = true;
    sOnline->fixedRoom = false;
    sOnline->dropConn = true; /* leave the current room: the thread says hello again */
    LightLock_Unlock(&sOnline->lock);
    LightEvent_Signal(&sOnline->wake);
}

void Online_LeaveRoom(void)
{
    if (!sOnline)
        return;
    LightLock_Lock(&sOnline->lock);
    sOnline->room[0] = '\0';
    sOnline->roomState = ONLINE_ROOM_NONE;
    sOnline->paused = true;
    sOnline->status[0] = '\0';
    LightLock_Unlock(&sOnline->lock);
    ShutdownConnection();
    LightEvent_Signal(&sOnline->wake);
}

/* The room's kind (ONLINE_KIND_*); after ONLINE_ROOM_WRONG_KIND, the kind of
 * the room the code was for. */
uint8_t Online_GetRoomKind(void)
{
    u8 kind;

    if (!sOnline)
        return ONLINE_KIND_ANY;
    LightLock_Lock(&sOnline->lock);
    kind = sOnline->roomState == ONLINE_ROOM_WRONG_KIND ? sOnline->wrongKind : sOnline->kind;
    LightLock_Unlock(&sOnline->lock);
    return kind;
}

uint8_t Online_GetRoom(char *code, uint32_t size)
{
    u8 state;

    if (!sOnline)
    {
        if (size)
            code[0] = '\0';
        return ONLINE_ROOM_OFF;
    }
    LightLock_Lock(&sOnline->lock);
    state = sOnline->roomState;
    snprintf(code, size, "%s", state == ONLINE_ROOM_IN ? sOnline->room : "");
    LightLock_Unlock(&sOnline->lock);
    return state;
}

/* "wss://host[:port][/path]" or "ws://...": a WebSocket server. */
static void ParseWebSocketUrl(const char *url)
{
    const char *p = strstr(url, "://") + 3;
    const char *end = p + strcspn(p, ":/");
    u32 hostLen = end - p;

    sOnline->webSocket = true;
    sOnline->secure = strncasecmp(url, "wss", 3) == 0;
    if (hostLen >= sizeof(sOnline->host))
        hostLen = sizeof(sOnline->host) - 1;
    memcpy(sOnline->host, p, hostLen);
    sOnline->host[hostLen] = '\0';
    snprintf(sOnline->port, sizeof(sOnline->port), "%s", sOnline->secure ? "443" : "80");
    if (*end == ':')
        snprintf(sOnline->port, sizeof(sOnline->port), "%d", atoi(end + 1));
    end = strchr(end, '/');
    snprintf(sOnline->path, sizeof(sOnline->path), "%s", end ? end : "/");
}

void Online_Init(const char *server, const char *room)
{
    s32 priority = 0x30;

    if (sOnline || !server || !*server)
        return;
    sOnline = calloc(1, sizeof(*sOnline));
    if (!sOnline)
        return;
    if (strstr(server, "://"))
    {
        ParseWebSocketUrl(server);
    }
    else
    {
        const char *colon = strrchr(server, ':');
        u32 hostLen = colon ? (u32)(colon - server) : strlen(server);

        if (hostLen >= sizeof(sOnline->host))
            hostLen = sizeof(sOnline->host) - 1;
        memcpy(sOnline->host, server, hostLen);
        snprintf(sOnline->port, sizeof(sOnline->port), "%s", colon ? colon + 1 : "7777");
    }
    /* Not in a room (or connected) until the player makes or joins one. */
    sOnline->roomState = ONLINE_ROOM_NONE;
    sOnline->paused = true;
    LightLock_Init(&sOnline->lock);
    LightEvent_Init(&sOnline->wake, RESET_ONESHOT);
    /* Below the game: the thread mostly waits on the network, and runs while
     * the game waits for the next frame. */
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    sOnline->thread = threadCreate(OnlineThread, NULL, THREAD_STACK, priority < 0x3F ? priority + 1 : 0x3F,
                                   -2, false);
    if (sOnline->thread == NULL)
    {
        CtrLog_Write(CTR_LOG_ERROR, "online: no thread, online play off");
        free(sOnline);
        sOnline = NULL;
        return;
    }
    CtrLog_Write(CTR_LOG_GAME, "online: server %s port %s%s", sOnline->host, sOnline->port,
                 sOnline->webSocket ? (sOnline->secure ? " (wss)" : " (ws)") : "");
    if (room && *room)
    {
        Online_JoinRoom(room, ONLINE_JOIN_OR_CREATE, ONLINE_KIND_ANY);
        sOnline->fixedRoom = true;
    }
}

void Online_Shutdown(void)
{
    if (!sOnline)
        return;
    LightLock_Lock(&sOnline->lock);
    sOnline->quit = true;
    sOnline->dropConn = true;
    LightLock_Unlock(&sOnline->lock);
    LightEvent_Signal(&sOnline->wake);
    /* A connect in progress can't be cut short; past that, leave it. */
    if (R_FAILED(threadJoin(sOnline->thread, 2000000000LL)))
        return;
    threadFree(sOnline->thread);
    sOnline->thread = NULL;
    if (sSocReady)
    {
        socExit();
        if (sSslcReady)
            sslcExit();
        else
            psExit();
        sSocReady = false;
    }
}

uint8_t Online_IsRoomFixed(void)
{
    return sOnline && sOnline->fixedRoom;
}

uint8_t Online_InRoom(void)
{
    u8 state;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    state = sOnline->roomState;
    LightLock_Unlock(&sOnline->lock);
    return state == ONLINE_ROOM_IN || state == ONLINE_ROOM_CONNECTING;
}

uint8_t Online_Enabled(void)
{
    return sOnline != NULL;
}

uint8_t Online_Connected(void)
{
    bool connected;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    connected = sOnline->connected;
    LightLock_Unlock(&sOnline->lock);
    return connected;
}

uint16_t Online_MyId(void)
{
    u16 id;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    id = sOnline->connected ? sOnline->myId : 0;
    LightLock_Unlock(&sOnline->lock);
    return id;
}

uint32_t Online_Generation(void)
{
    u32 gen;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    gen = sOnline->generation;
    LightLock_Unlock(&sOnline->lock);
    return gen;
}

void Online_GetStatus(char *out, uint32_t size)
{
    if (!sOnline)
    {
        if (size)
            out[0] = '\0';
        return;
    }
    LightLock_Lock(&sOnline->lock);
    snprintf(out, size, "%s", sOnline->status);
    LightLock_Unlock(&sOnline->lock);
}

void Online_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data)
{
    u8 advert[2 + ONLINE_ADVERT_SIZE];
    bool changed;

    if (!sOnline)
        return;
    advert[0] = hosting ? 1 : 0;
    advert[1] = nextSlot;
    if (data)
        memcpy(advert + 2, data, ONLINE_ADVERT_SIZE);
    LightLock_Lock(&sOnline->lock);
    changed = sOnline->haveAdvert != (data != NULL)
           || (data && memcmp(sOnline->advert, advert, sizeof(advert)) != 0);
    sOnline->haveAdvert = (data != NULL);
    if (data)
        memcpy(sOnline->advert, advert, sizeof(advert));
    if (changed && sOnline->connected)
        QueueFrame(C_ADVERT, advert, data ? sizeof(advert) : 0, NULL, 0);
    LightLock_Unlock(&sOnline->lock);
}

int Online_GetPeers(struct OnlinePeer *out, int max)
{
    int n = 0;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    for (int i = 0; i < ONLINE_MAX_PEERS && n < max; i++)
    {
        if (!sOnline->peers[i].valid)
            continue;
        out[n] = sOnline->peers[i];
        if (out[n].hosting)
            out[n].hostedAgoMs = 0;
        else if (out[n].lastHostingMs == 0)
            out[n].hostedAgoMs = 0xFFFFFFFF; /* never hosted */
        else
            out[n].hostedAgoMs = Online_Milliseconds() - out[n].lastHostingMs;
        n++;
    }
    LightLock_Unlock(&sOnline->lock);
    return n;
}

void Online_Send(uint16_t dest, const void *data, uint32_t len)
{
    u8 to[2];

    if (!sOnline)
        return;
    to[0] = dest;
    to[1] = dest >> 8;
    LightLock_Lock(&sOnline->lock);
    if (sOnline->connected)
        QueueFrame(C_SEND, to, 2, data, len);
    LightLock_Unlock(&sOnline->lock);
}

uint8_t Online_PollEvent(struct OnlineEvent *ev)
{
    bool got = false;

    if (!sOnline)
        return 0;
    LightLock_Lock(&sOnline->lock);
    if (sOnline->eventHead != sOnline->eventTail)
    {
        *ev = sOnline->events[sOnline->eventHead];
        sOnline->eventHead = (sOnline->eventHead + 1) % EVENT_QUEUE;
        got = true;
    }
    LightLock_Unlock(&sOnline->lock);
    return got;
}
