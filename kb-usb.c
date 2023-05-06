#include "kb-usb.h"
#include <stdlib.h>

static USBBuffer data_rx_urb;
static USBBuffer data_tx_urb[MAX_TX_URB];
static uint8_t usb_rx_data[USB_EP3_SIZE];
static uint8_t enabled = 0;

/* EP3 OUT rx buffer. */
static uint8_t rx_buffer[RX_BUFSIZE];
volatile int ptr;

/* EP2 IN tx buffer. */
static uint8_t tx_buffer[MAX_TX_URB][USB_EP2_SIZE];
static uint8_t xmit_buf[USB_EP2_SIZE];

/* EP2 packet FIFO */
volatile uint8_t PKT_FIFO[PKT_FIFO_MAX];
volatile int pkt_fifo_size = 0;

volatile kb_usb_state g_state;
volatile kb_event_t *kb_event;
volatile int g_pkt_len;

/* Custom event message. */
process_event_t kb_event_message;

/* Custom event message. */
process_event_t kb_sendpkt_message;

static void append_buffer_to_fifo(uint8_t *buffer, size_t len) {
    if ((pkt_fifo_size + len) <= (PKT_FIFO_MAX)) {
        for (size_t i = 0; i < len; i++) {
            PKT_FIFO[pkt_fifo_size + i] = buffer[i];
        }
        pkt_fifo_size += len;
    }
}

void kb_usb_send_bytes(uint8_t *bytes, int length) {
    int i, len, pkt_idx;

    /* Don't send if not enabled. */
    if (!enabled) {
        return;
    }

    append_buffer_to_fifo(bytes, length);

    /* Can we send a single packet with some padding ? */
    pkt_idx = 0;
    while ((pkt_fifo_size >= USB_EP2_SIZE) && (pkt_idx < MAX_TX_URB)) {
        len = USB_EP2_SIZE - 1;
        memcpy(&tx_buffer[pkt_idx][1], PKT_FIFO, len);
        tx_buffer[pkt_idx][0] = len;
        data_tx_urb[pkt_idx].flags = USB_BUFFER_IN;
        data_tx_urb[pkt_idx].flags |= USB_BUFFER_NOTIFY;
        data_tx_urb[pkt_idx].data = tx_buffer[pkt_idx];
        data_tx_urb[pkt_idx].left = USB_EP2_SIZE;
        if (pkt_idx > 0)
            data_tx_urb[pkt_idx - 1].next = &data_tx_urb[pkt_idx];
        else
            data_tx_urb[pkt_idx].next = NULL;
        //usb_submit_xmit_buffer(EPIN, &data_tx_urb);

        /* Chomp data. */
        for (i = len; i < pkt_fifo_size; i++) {
            PKT_FIFO[i - len] = PKT_FIFO[i];
        }
        pkt_fifo_size -= len;
        pkt_idx++;
    }

    if ((pkt_fifo_size < USB_EP2_SIZE) && (pkt_idx < MAX_TX_URB)) {
        len = pkt_fifo_size;
        memcpy(&tx_buffer[pkt_idx][1], PKT_FIFO, len);
        tx_buffer[pkt_idx][0] = len;
        data_tx_urb[pkt_idx].flags = USB_BUFFER_IN;
        data_tx_urb[pkt_idx].flags |= USB_BUFFER_NOTIFY;
        data_tx_urb[pkt_idx].data = tx_buffer[pkt_idx];
        data_tx_urb[pkt_idx].left = USB_EP2_SIZE;
        data_tx_urb[pkt_idx].next = NULL;
        if (pkt_idx > 0)
            data_tx_urb[pkt_idx - 1].next = &data_tx_urb[pkt_idx];
        else
            data_tx_urb[pkt_idx].next = NULL;
        pkt_fifo_size = 0;
    }
    usb_submit_xmit_buffer(EPIN, &data_tx_urb[0]);
}

/* Callback to the input handler */
static void input_handler(unsigned char c) {
    switch (g_state) {
        case KBS_IDLE: {
            /* Packet length must be >= 3. */
            if (c < 3)
                break;

            /* Parse length byte. */
            g_pkt_len = c;
            ptr = 0;
            rx_buffer[ptr++] = (uint8_t) c;
            g_state = KBS_WAIT_PAYLOAD;
        }
            break;

        case KBS_WAIT_PAYLOAD: {
            if ((ptr <= g_pkt_len) && (ptr < RX_BUFSIZE)) {
                rx_buffer[ptr++] = (uint8_t) c;
            }

            if (ptr == g_pkt_len) {
                //rx_buffer[ptr++] = (uint8_t)c;
                g_state = KBS_PACKET_RECEIVED;

                /* Check CRC. */
                if (packet_is_valid(rx_buffer, g_pkt_len)) {
                    /* Allocate a KB event. */
                    kb_event = (kb_event_t *) malloc(sizeof(kb_event_t));
                    if (kb_event != NULL) {
                        /* Command at offset 1. */
                        kb_event->command = rx_buffer[1];
                        kb_event->payload_size = g_pkt_len - 3;

                        /* Allocate a new payload buffer. */
                        kb_event->payload = (uint8_t *) malloc(sizeof(uint8_t) * kb_event->payload_size);
                        if (kb_event->payload != NULL) {
                            /* Copy payload. */
                            memcpy(kb_event->payload, &rx_buffer[2], kb_event->payload_size);
                        } else {
                            kb_event->payload = NULL;
                        }

                        /* Broadcast event */
                        process_post(PROCESS_BROADCAST, kb_event_message, (void *) kb_event);
                    }
                }

                /* Wait for another packet. */
                g_state = KBS_IDLE;
            }
        }
            break;

        default:
            g_state = KBS_IDLE;
            break;

    }
}

const struct usb_st_device_descriptor device_descriptor =
        {
                sizeof(struct usb_st_device_descriptor),
                DEVICE,
                0x0200,
                0, /* Defined at interface level. */
                0,
                0,
                CTRL_EP_SIZE,
                CDC_ACM_CONF_VID,
                CDC_ACM_CONF_PID,
                0x0000,
                1,
                2,
                0,
                1
        };

const struct configuration_st {
    struct usb_st_configuration_descriptor configuration;
    struct usb_st_interface_descriptor data;
    struct usb_st_endpoint_descriptor ep_in;
    struct usb_st_endpoint_descriptor ep_out;
} BYTE_ALIGNED
configuration_block =
        {
                /* Configuration */
                {
                        sizeof(configuration_block.configuration),
                        CONFIGURATION,
                        sizeof(configuration_block),
                        1, /* bNumInterfaces = 1 */
                        1, /* bConfigurationValue */
                        0, /* iConfiguration */
                        0x80, /* Bus powered */
                        250 /* 250 mA */
                },
                {
                        sizeof(configuration_block.data),
                        INTERFACE,
                        0, /* Interface number = 0 */
                        0, /* bAlternate setting = 0 */
                        2, /* bNumEndpoints = 2 */
                        0xff, /* Interface class = 0xFF (vendor class specific) */
                        0, /* bInterface subclass */
                        0, /* bInterface protocol */
                        0 /* iInterface */
                },
                {
                        sizeof(configuration_block.ep_in),
                        ENDPOINT,
                        0x82,
                        0x02,
                        USB_EP2_SIZE,
                        0
                },
                {
                        sizeof(configuration_block.ep_out),
                        ENDPOINT,
                        0x03,
                        0x02,
                        USB_EP3_SIZE,
                        0
                }
        };

const struct usb_st_configuration_descriptor const *configuration_head =
        (struct usb_st_configuration_descriptor const *) &configuration_block;


static void
queue_rx_urb(void) {
    data_rx_urb.flags = USB_BUFFER_PACKET_END;    /* Make sure we are getting immediately the packet. */
    data_rx_urb.flags |= USB_BUFFER_NOTIFY;
    data_rx_urb.data = usb_rx_data;
    data_rx_urb.left = USB_EP3_SIZE;
    data_rx_urb.next = NULL;
    usb_submit_recv_buffer(EPOUT, &data_rx_urb);
}

static void
do_work(void) {
    unsigned int events;

    events = usb_get_global_events();
    if (events & USB_EVENT_CONFIG) {

        /* Force state once the device is configured. */
        g_state = KBS_IDLE;
        ptr = 0;
        pkt_fifo_size = 0;

        /* Enable endpoints */
        enabled = 1;
        usb_setup_bulk_endpoint(EPIN);
        usb_setup_bulk_endpoint(EPOUT);

        queue_rx_urb();
    }
    if (events & USB_EVENT_RESET) {
        enabled = 0;
    }

    if (!enabled) {
        return;
    }

    events = usb_get_ep_events(EPOUT);
    if ((events & USB_EP_EVENT_NOTIFICATION)
        && !(data_rx_urb.flags & USB_BUFFER_SUBMITTED)) {
        if (!(data_rx_urb.flags & USB_BUFFER_FAILED)) {
            int len;
            int i;

            len = BUFFER_SIZE - data_rx_urb.left;
            for (i = 0; i < len; i++) {
                input_handler(usb_rx_data[i]);
            }
        }
        queue_rx_urb();
    }
}

PROCESS(kb_usb_process,
"Killerbee USB driver");
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(kb_usb_process, ev, data
)
{
kb_event_t *p_sendpkt;

PROCESS_BEGIN();

leds_on(LEDS_GREEN);

kb_event_message = process_alloc_event();
kb_sendpkt_message = process_alloc_event();
ptr = 0;
pkt_fifo_size = 0;

usb_set_global_event_process(&kb_usb_process);
usb_set_ep_event_process(EPIN, &kb_usb_process);
usb_set_ep_event_process(EPOUT, &kb_usb_process);

while(1) {
PROCESS_WAIT_EVENT();

if(ev == PROCESS_EVENT_EXIT) {
break;
}

/* Do we have a packet to send ? */
if (ev == kb_sendpkt_message) {
leds_on(LEDS_RED);

/* Got packet to send, send packet through USB endpoint 2 */
p_sendpkt = (kb_event_t *) data;
kb_usb_send_bytes(p_sendpkt
->payload, p_sendpkt->payload_size);

/* Free packet resources. */
free(p_sendpkt
->payload);
free(p_sendpkt);
leds_off(LEDS_RED);
}

if(ev == PROCESS_EVENT_POLL) {
do_work();

}
}

leds_off(LEDS_GREEN);

PROCESS_END();

}

void kb_fifo_reset(void) {
    memset(PKT_FIFO, 0, PKT_FIFO_MAX);
    pkt_fifo_size = 0;
}

void kb_usb_reset(void) {
    /* Initialize state. */
    g_state = KBS_IDLE;
    ptr = 0;

    /* Reset data FIFO. */
    kb_fifo_reset();
}

void kb_usb_init(void) {
    /* Reset USB processing globals and FIFO. */
    kb_usb_reset();

    /* Switch off LEDs. */
    leds_off(LEDS_RED);

    /* Start USB task. */
    process_start(&kb_usb_process, NULL);
}
