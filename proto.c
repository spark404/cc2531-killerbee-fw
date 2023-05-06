#include "proto.h"
#include <stdlib.h>

extern process_event_t kb_sendpkt_message;

void proto_send_packet(packet_t *p_pkt) {
    proto_send(CMD_GOT_PKT, p_pkt->payload, p_pkt->size);

    /* Free packet. */
    free(p_pkt);
}

void proto_send(command_t command, uint8_t *payload, int len) {
    kb_event_t *p_kb_evt;
    int i;
    uint8_t checksum;

    /* Allocate memory for kb_event_t structure. */
    p_kb_evt = (kb_event_t *) malloc(sizeof(kb_event_t));
    if (p_kb_evt == NULL) {
        // OOM Panic?
        return;
    }

    /* Allocate memory for our payload. */
    p_kb_evt->payload = (uint8_t *) malloc(len + 3);
    if (p_kb_evt->payload == NULL) {
        // OOM Panic?
        return;
    }

    /* Build our event. */
    p_kb_evt->payload[0] = (uint8_t)(len + 3);
    p_kb_evt->payload[1] = command;
    for (i = 0; i < len; i++) {
        p_kb_evt->payload[2 + i] = payload[i];
    }
    checksum = packet_compute_checksum(p_kb_evt->payload, len + 2);
    p_kb_evt->payload[2 + i] = checksum;
    p_kb_evt->payload_size = len + 3;

    /* Send packet to USB process. */
    process_post(PROCESS_BROADCAST, kb_sendpkt_message, (void *) p_kb_evt);
}