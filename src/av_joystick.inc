/* Included in the game-local XRoar joystick backend. Physical machines use
 * the same guest PIA/DAC routine and have no host input descriptor. */
static unsigned char av_pointer[4] = {32, 24, 0};

#if !defined(WINDOWS32) && !defined(HAVE_WASM)
#include "events.h"
#include "keyboard.h"
#include "xroar.h"
#include <sys/socket.h>

static int av_pointer_fd = -2;
static int av_pointer_started;
static uint32_t av_next_poll, av_edge_until;
static int av_holding_edge;

static int av_poll_pointer(void) {
    if (av_pointer_fd == -2) {
        const char *value = getenv("COCOWAR_POINTER_FD");
        av_pointer_fd = value ? atoi(value) : -1;
    }
    if (av_pointer_fd < 0) return 0;
    if (av_pointer_started && (int32_t)(event_current_tick-av_next_poll)<0) return 1;
    av_pointer_started=1;
    av_next_poll=event_current_tick+EVENT_MS(2);
    unsigned char packet[4];
    if (av_holding_edge && (int32_t)(event_current_tick-av_edge_until)>=0)
        av_holding_edge=0;
    for (;;) {
        if (av_holding_edge) {
            if (recv(av_pointer_fd,packet,sizeof packet,MSG_DONTWAIT|MSG_PEEK)!=4) break;
            if (packet[2]!=av_pointer[2] || packet[3]!=av_pointer[3]) break;
        }
        if (recv(av_pointer_fd,packet,sizeof packet,MSG_DONTWAIT)!=4) break;
        int edge=packet[2]!=av_pointer[2] || packet[3]!=av_pointer[3];
        static const unsigned char keys[] = {DSCAN_G, DSCAN_Q, DSCAN_Y, DSCAN_N, DSCAN_S, DSCAN_R, DSCAN_T};
        for (unsigned i=0;i<sizeof keys && xroar.keyboard_interface;i++) {
            unsigned mask=1u<<i;
            if (!((packet[3]^av_pointer[3])&mask)) continue;
            if (packet[3]&mask) keyboard_press(xroar.keyboard_interface,keys[i]);
            else keyboard_release(xroar.keyboard_interface,keys[i]);
        }
        int v_changed=(packet[2]^av_pointer[2]) & 8;
        int shift_changed=(packet[2]^av_pointer[2]) & 16;
        static const unsigned char build_keys[] = {DSCAN_B, DSCAN_CLEAR, DSCAN_BREAK};
        for (unsigned i=0;i<3 && xroar.keyboard_interface;i++) {
            unsigned mask=32u<<i;
            if (!((packet[2]^av_pointer[2])&mask)) continue;
            if (packet[2]&mask) keyboard_press(xroar.keyboard_interface,build_keys[i]);
            else keyboard_release(xroar.keyboard_interface,build_keys[i]);
        }
        if (((packet[2]^av_pointer[2])&2) && xroar.keyboard_interface) {
            if (packet[2]&2) keyboard_press(xroar.keyboard_interface,DSCAN_C);
            else keyboard_release(xroar.keyboard_interface,DSCAN_C);
        }
        memcpy(av_pointer,packet,sizeof packet);
        if (shift_changed && (packet[2]&16) && xroar.keyboard_interface)
            keyboard_press(xroar.keyboard_interface,DSCAN_SHIFT);
        if (v_changed && xroar.keyboard_interface) {
            if (packet[2]&8) keyboard_press(xroar.keyboard_interface,DSCAN_V);
            else keyboard_release(xroar.keyboard_interface,DSCAN_V);
        }
        if (shift_changed && !(packet[2]&16) && xroar.keyboard_interface)
            keyboard_release(xroar.keyboard_interface,DSCAN_SHIFT);
        if (edge) {
            /* Even a short desktop click must span one 15 Hz guest scan. */
            av_edge_until=event_current_tick+EVENT_MS(85);
            av_holding_edge=1;
            break;
        }
    }
    return 1;
}

#else
static int av_poll_pointer(void) { return 0; }
#endif
