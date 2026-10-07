#ifndef CTR_ONLINE_H
#define CTR_ONLINE_H

/*
 * Connection to the online relay server (3ds_online.c), used by the emulated
 * Wireless Adapter (3ds_rfu_adapter.c). The same server and protocol as the
 * PC port's, so the two play together.
 *
 * Native, but the adapter and the attendants' room menu (src/online_room.c)
 * are game units: only fixed-size types here.
 */
#include <stdint.h>

#define ONLINE_MAX_PEERS 8     /* a room holds 5; room to spare */
#define ONLINE_ADVERT_SIZE 24  /* a Wireless Adapter's broadcast data */
/* The server the settings start with (settings.txt, online_server=). */
#define ONLINE_DEFAULT_SERVER "wss://pokemon.eathan.page"

struct OnlinePeer
{
    uint8_t valid;
    uint8_t hasAdvert;
    uint8_t hosting;
    uint8_t nextSlot;
    uint16_t id;
    uint8_t advert[ONLINE_ADVERT_SIZE];
    uint32_t lastHostingMs; /* when it was last hosting, 0 if never */
    uint32_t hostedAgoMs;   /* filled in by Online_GetPeers: 0 if hosting now */
};

enum
{
    ONLINE_EV_RECV, /* data from peer `id` */
    ONLINE_EV_LEFT, /* peer `id` left the room (or we lost the connection) */
};

struct OnlineEvent
{
    uint8_t type;
    uint16_t id;
    uint16_t len;
    uint8_t data[160];
};

/* server: "wss://host[:port][/path]", "ws://..." or "host[:port]" (plain TCP,
 * port 7777 if left out); NULL or "" leaves online off. room: a code to join
 * at once (tests); NULL or "": none until the player makes or joins one. */
void Online_Init(const char *server, const char *room);
/* At exit: the connection is closed and the network services let go. */
void Online_Shutdown(void);
uint8_t Online_Enabled(void);
uint8_t Online_Connected(void);
uint16_t Online_MyId(void);       /* our device id in the room; 0 if not connected */
uint32_t Online_Generation(void); /* changes on every (re)connection */
void Online_GetStatus(char *out, uint32_t size); /* e.g. "Online room ABC234" */
/* Our adapter's broadcast while hosting (data NULL: none). */
void Online_SetAdvert(uint8_t hosting, uint8_t nextSlot, const uint8_t *data);
int Online_GetPeers(struct OnlinePeer *out, int max);
void Online_Send(uint16_t dest, const void *data, uint32_t len);
uint8_t Online_PollEvent(struct OnlineEvent *ev);
uint32_t Online_Milliseconds(void);

/* Rooms. Online_JoinRoom leaves the current room and joins `room` (NULL or
 * "": a new room with a code from the server); Online_GetRoom says how that
 * went, with the code once in. */
enum
{
    ONLINE_JOIN_OR_CREATE, /* join, or make the room if nobody's in it */
    ONLINE_JOIN_ONLY,      /* join only if it exists */
};
void Online_JoinRoom(const char *room, uint8_t mode, uint8_t kind); /* kind: ONLINE_KIND_* */
uint8_t Online_GetRoomKind(void);
void Online_LeaveRoom(void);
uint8_t Online_IsRoomFixed(void); /* the room was given at startup (tests) */
uint8_t Online_InRoom(void);      /* in an online room, or joining one */
uint8_t Online_GetRoom(char *code, uint32_t size); /* ONLINE_ROOM_* */

#endif /* CTR_ONLINE_H */
