//
// Copyright(C) 2021 Chris Marchetti <adamnew123456@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#include "doomkeys.h"
#include "d_event.h"
#include "i_system.h"
#include "i_vnc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define VNC_CLIENT_SETPIXELFORMAT 0
#define VNC_CLIENT_SETENCODINGS 2
#define VNC_CLIENT_FRAMEBUFFERUPDATEREQUEST 3
#define VNC_CLIENT_KEYEVENT 4
#define VNC_CLIENT_POINTEREVENT 5
#define VNC_CLIENT_CLIENTCUTTEXT 6

#define VNC_SERVER_FRAMEBUFFERUPDATE 0
#define VNC_SERVER_SETCOLORMAPENTRIES 1

static const char vnc_keysym_unshifted[] = {
    // Control characters, these have no meaningful casing
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, // Space
    '1', // !
    '\'', // "
    '3', // #
    '4', // $
    '5', // %
    '7', // &
    0, // '
    '9', // (
    '0', // )
    '8', // *
    '=', // +
    0, // ,
    0, // -
    0, // .
    0, // /
    // Numerics are all their own lower casing
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    ';', // :
    0, // ;
    ',', // <
    0, // =
    '.', // >
    '/', // ?
    '2', // @
    // Upper case alphabet
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    0, // [
    0, // backspace
    0, // ]
    '6', // ^
    '-', // _
    0, // `
    // Lower case alphabet
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '[', // {
    '\\', // |
    ']', // }
    '`', // ~
    0, // DEL
};

static int SendAll(int sock, byte* buffer, int size)
{
    int offset = 0;
    while (offset < size)
    {
        int chunk = send(sock, buffer + offset, size - offset, 0);
        if (chunk <= 0)
        {
            // Client disconnected or something else unusual happened
            return -1;
        }

        offset += chunk;
    }

    return 0;
}

static int RecvAll(int sock, byte* buffer, int size)
{
    int offset = 0;
    while (offset < size)
    {
        int chunk = recv(sock, buffer + offset, size - offset, 0);
        if (chunk <= 0)
        {
            // Client disconnected or something else unusual happened
            return -1;
        }

        offset += chunk;
    }

    return 0;
}

static int FinalizeVNCMessages(vnc_server_t *server, int offset)
{
    byte* leftover_data = server->client_packet + offset;
    int leftover_size = server->packet_cursor - offset;

    if (leftover_size > 0)
    {
        // *Hopefully* this is smart enough to figure out that it can safely copy
        // through the array backwards without having to use scratch space
        memmove(server->client_packet, leftover_data, leftover_size);
    }

    server->packet_cursor = leftover_size;
}

static int HandleVNCMessage(vnc_server_t *server, int message_scan_pos, int* cursor_x, int* cursor_y, int* mouse_buttons)
{
    byte* packet_base = server->client_packet + message_scan_pos;
    int data_left = server->packet_cursor - message_scan_pos;
    if (data_left == 0) return -1;

    switch (packet_base[0])
    {
        case VNC_CLIENT_SETPIXELFORMAT:
            {
                byte px_size = packet_base[4];
                byte true_color = packet_base[7];

                if (data_left >= 20)
                {
                    if (!true_color)
                    {
                      printf("HandleVNCMessage: Unsupported palette color mode\n");
                      VNC_Exit(server);
                      I_Quit();
                      return -1;
                    }

                    if (px_size != 32)
                    {
                        printf("HandleVNCMessage: Unsupported pixel size mode: %d\n", px_size);
                        VNC_Exit(server);
                        I_Quit();
                        return -1;
                    }

                    return message_scan_pos + 20;
                }
            }
            break;

        case VNC_CLIENT_SETENCODINGS:
            if (server->packet_cursor - message_scan_pos >= 4)
            {
                // We can figure out the true size by looking at the encoding count
                int encoding_count = (packet_base[2] << 8) | packet_base[3];
                int expect_length = 4 + encoding_count * 4;

                boolean contains_tight = false;
                int encoding_offset = 4;
                for (int i = 0; i < encoding_count; i++)
                {
                    int encoding =
                        (packet_base[encoding_offset] << 24) |
                        (packet_base[encoding_offset + 1] << 16) |
                        (packet_base[encoding_offset + 2] << 8) |
                        packet_base[encoding_offset + 3];

                    encoding_offset += 4;
                    if (encoding == VNC_TIGHT)
                    {
                        contains_tight = true;
                        break;
                    }
                }

                if (data_left >= expect_length)
                {
                    if (contains_tight)
                    {
                        server->encoding = VNC_TIGHT;
                    }
                    else
                    {
                        server->encoding = VNC_RAW;
                    }

                    return message_scan_pos + expect_length;
                }
            }
            break;

        case VNC_CLIENT_FRAMEBUFFERUPDATEREQUEST:
            if (data_left >= 10)
            {
                server->send_frame = 1;
                return message_scan_pos + 10;
            }
            break;

        case VNC_CLIENT_KEYEVENT:
            {
                byte is_keydown = packet_base[1];
                int keysym =
                    (packet_base[4] << 24) |
                    (packet_base[5] << 16) |
                    (packet_base[6] << 8) |
                    packet_base[7];
                boolean is_key_known = keysym <= 0x7f;

                if (data_left >= 8)
                {
                    event_t event;
                    event.data2 = 0;
                    event.data3 = 0;
                    event.type = is_keydown ? ev_keydown : ev_keyup;

                    // According to the transforms done by the SDL code
                    // from i_input (prior to deleting it), data1, data2
                    // and data3 are used this way:
                    //
                    // - data1 contains a "translated key", which uses either
                    //   ASCII characters or the keysyms defined in doomkeys.h
                    //
                    // - data2 contains a "localized key", which is a character
                    //   in the user's keyboard layout but without any shifting
                    //   or other modifiers applied.
                    //
                    // - data3 contains a "typed character", which is a character
                    //   composed of the key code, the user's keyboard layout and
                    //   any modifiers. It is also Unicode aware.
                    //
                    // RFC 6143 is picky about the interpretation of key events,
                    // but they correspond most closely to typed characters since
                    // they recognize Unicode. We're just going to munge these
                    // concepts into one thing and pretend that the only keyboard
                    // layout is the one sitting in front of me (a US layout tenkeyless).
                    //
                    // Note that we have to "unshift" keys in order to generate a key
                    // which corresponds to the key the user hit, rather than the character
                    // they typed
                    switch (keysym)
                    {
                        case 0xff1b: // Escape has a real ASCII mapping
                            keysym = 0x1b;
                            is_key_known = 1;
                            break;
                        case 0xff08: // As does backspace
                            keysym = 0x08;
                            is_key_known = 1;
                            break;
                        case 0xff09: // As does tab
                            keysym = 0x09;
                            is_key_known = 1;
                            break;
                        case 0xff0d: // As does enter
                            keysym = 0x0d;
                            is_key_known = 1;
                            break;
                        case 0xffff: // As does delete
                            keysym = 0x1b;
                            is_key_known = 1;
                            break;
                        case 0xffbe: // Function keys just map to doomkeys
                            keysym = KEY_F1;
                            is_key_known = 1;
                            break;
                        case 0xffbf:
                            keysym = KEY_F2;
                            is_key_known = 1;
                            break;
                        case 0xffc0:
                            keysym = KEY_F3;
                            is_key_known = 1;
                            break;
                        case 0xffc1:
                            keysym = KEY_F4;
                            is_key_known = 1;
                            break;
                        case 0xffc2:
                            keysym = KEY_F5;
                            is_key_known = 1;
                            break;
                        case 0xffc3:
                            keysym = KEY_F6;
                            is_key_known = 1;
                            break;
                        case 0xffc4:
                            keysym = KEY_F7;
                            is_key_known = 1;
                            break;
                        case 0xffc5:
                            keysym = KEY_F8;
                            is_key_known = 1;
                            break;
                        case 0xffc6:
                            keysym = KEY_F9;
                            is_key_known = 1;
                            break;
                        case 0xffc7:
                            keysym = KEY_F10;
                            is_key_known = 1;
                            break;
                        case 0xffc8:
                            keysym = KEY_F11;
                            is_key_known = 1;
                            break;
                        case 0xffc9:
                            keysym = KEY_F12;
                            is_key_known = 1;
                            break;
                        case 0xff51: // Left
                            keysym = KEY_LEFTARROW;
                            is_key_known = 1;
                            break;
                        case 0xff52: // Up
                            keysym = KEY_UPARROW;
                            is_key_known = 1;
                            break;
                        case 0xff53: // Right
                            keysym = KEY_RIGHTARROW;
                            is_key_known = 1;
                            break;
                        case 0xff54: // Down
                            keysym = KEY_DOWNARROW;
                            is_key_known = 1;
                            break;
                        case 0xff13: // Pause
                            keysym = KEY_PAUSE;
                            is_key_known = 1;
                            break;
                        case 0xffe1: // Left shift
                        case 0xffe2: // Right shift
                            keysym = KEY_RSHIFT;
                            is_key_known = 1;
                            break;
                        case 0xffe3: // Left control
                        case 0xffe4: // Right control
                            keysym = KEY_RCTRL;
                            is_key_known = 1;
                            break;
                        case 0xffe9: // Left alt
                        case 0xffea: // Right alt
                            keysym = KEY_RALT;
                            is_key_known = 1;
                            break;
                        case 0xffe5: // Caps lock
                            keysym = KEY_CAPSLOCK;
                            is_key_known = 1;
                            break;
                        case 0xff14: // Scroll lock
                            keysym = KEY_SCRLCK;
                            is_key_known = 1;
                            break;
                        case 0xff7f: // Num lock
                            keysym = KEY_NUMLOCK;
                            is_key_known = 1;
                            break;
                        case 0xff61: // Print screen
                            keysym = KEY_PRTSCR;
                            is_key_known = 1;
                            break;
                        case 0xff50: // Home
                            keysym = KEY_HOME;
                            is_key_known = 1;
                            break;
                        case 0xff57: // End
                            keysym = KEY_END;
                            is_key_known = 1;
                            break;
                        case 0xff55: // Page up
                            keysym = KEY_PGUP;
                            is_key_known = 1;
                            break;
                        case 0xff56: // Page down
                            keysym = KEY_PGDN;
                            is_key_known = 1;
                            break;
                        case 0xff63: // Insert
                            keysym = KEY_INS;
                            is_key_known = 1;
                            break;
                    }

                    // A key definition of some kind, but not one that maps to
                    // anything on a US layout, at least directly. Don't bother
                    // sending these along.
                    if (!is_key_known)
                    {
                      return message_scan_pos + 8;
                    }

                    int keysym_lowercase = keysym;
                    if (keysym <= 0x7f && vnc_keysym_unshifted[keysym] != 0)
                    {
                        keysym_lowercase = vnc_keysym_unshifted[keysym];
                    }

                    event.data1 = keysym_lowercase;
                    if (is_keydown)
                    {
                        event.data2 = keysym_lowercase;
                        if (server->text_input)
                        {
                            // Usually we're dealing with raw key codes (or
                            // at least the closest we're able to emulate
                            // here), but for text input we do actually send
                            // the original along with no case adjustment
                            event.data3 = keysym;
                        }
                    }

                    D_PostEvent(&event);
                    return message_scan_pos + 8;
                }

                break;
            }

        case VNC_CLIENT_POINTEREVENT:
            {
                int x_pos = (packet_base[2] << 8) | packet_base[3];
                int y_pos = (packet_base[4] << 8) | packet_base[5];
                int left_button = packet_base[1] & 0x1;
                int right_button = packet_base[1] & 0x4;
                int middle_button = packet_base[1] & 0x2;
                int scroll_up = packet_base[1] & 0x8;
                int scroll_down = packet_base[1] & 0x10;

                if (data_left >= 6)
                {
                    *cursor_x = x_pos;
                    *cursor_y = y_pos;
                    *mouse_buttons = left_button
                        | (right_button << 1)
                        | (middle_button << 2)
                        | (scroll_up << 3)
                        | (scroll_down << 4);

                    // Defer this event so we can collect all mouse packets into a single event
                    return message_scan_pos + 6;
                }
                break;
            }

        case VNC_CLIENT_CLIENTCUTTEXT:
            // Length is dependent on data here, like SETENCODING
            if (data_left >= 8)
            {
                // XXX: This could easily overfill our buffer if the user sends data longer than VNC_PACKET_SIZE
                // The best we can hope for is that data greater than that has no control characters in it, so
                // that we can drop the buffer repeatedly and then resync at some later point
                int encoding_count =
                    (packet_base[4] << 24) |
                    (packet_base[5] << 16) |
                    (packet_base[6] << 8) |
                    packet_base[7];

                int expect_length = 8 + encoding_count;
                if (data_left >= expect_length)
                {
                    printf("%d[%d | %d | %d]\n", packet_base[0], server->packet_cursor, message_scan_pos, data_left);
                    return message_scan_pos + expect_length;
                }
            }

        default:
            // We couldn't understand the message, so just drop the buffer and
            // hopefully we'll resync at some point
            return -2;
    }

    return -1;
}

void VNC_Init(vnc_server_t* server, int width, int height)
{
    server->send_frame = false;
    server->server_packet = malloc(width * height * 32); // Allocate enough to fit even raw pixel data
    server->text_input = false;
    server->encoding = VNC_RAW;
    server->palette = NULL;
    server->mouse_x = 0;
    server->mouse_y = 0;
    server->width = width;
    server->height = height;

    // Wait for a client to show up. Make sure we say something so that the
    // user knows we're waiting for something and not just idling.
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in listen_addr = {0};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(5902);
    listen_addr.sin_addr.s_addr = 0;

    struct sockaddr_in peer_addr = {0};
    socklen_t peer_addrlen = {0};

    bind(server_sock, (const struct sockaddr*) &listen_addr, sizeof(struct sockaddr_in));
    listen(server_sock, 1);
    server->peer = -1;

    // Accept as many clients as we have to before one gives us the details we like
    while (1)
    {
        printf("VNC_Init: Waiting for connection\n");
        server->peer = accept(server_sock, (struct sockaddr*) &peer_addr, &peer_addrlen);
        if (server->peer == -1)
        {
            printf("VNC_Init: Failed to acquire client (%s)\n", strerror(errno));
            continue;
        }

        // Perform the VNC handshake. This is fairly simple, we just have to send over our version string,
        // auth info and initial state. A few things to watch out for:
        //
        // - The client sends a version lower than 3.8 (these have a different handshake mechanism, according
        //   to the RFC, which we don't want to support)
        //
        // - The client refuses to support non-authenticated connections.
        byte client_init_buffer[12];
        if (SendAll(server->peer, "RFB 003.008\n", 12))
        {
            printf("VNC_Init: Dropped client (could not send verstr)\n");
            goto client_abort;
        }

        if (RecvAll(server->peer, client_init_buffer, 12))
        {
            printf("VNC_Init: Dropped client (could not recv verstr)\n");
            goto client_abort;
        }

        if (strncmp("RFB 003.008\n", client_init_buffer, 12) != 0)
        {
            // Try to let the client know what's going on, if possible, before we kick them off
            // (0 security types; 13-byte reason string)
            SendAll(server->peer, "\x00\x00\x00\x00\x13Unsupported version", 18);
            printf("VNC_Init: Dropped client (invalid verstr: '%12.s')\n", client_init_buffer);
            goto client_abort;
        }

        printf("VNC_Init: Got good client version (%.11s)\n", client_init_buffer);
        // (1 security type; None)
        if (SendAll(server->peer, "\x01\x01", 2))
        {
            printf("VNC_Init: Dropped client (could not send auth types)\n");
            goto client_abort;
        }

        if (RecvAll(server->peer, client_init_buffer, 1))
        {
            printf("VNC_Init: Dropped client (did not receive auth type)\n");
            goto client_abort;
        }

        if (client_init_buffer[0] != 1)
        {
            // The client chose an illegal auth type, somehow
            // (status failed; 17-byte reason string)
            SendAll(server->peer, "\x00\x00\x00\x01\x00\x00\x00\x11Illegal auth type", 25);
            printf("VNC_Init: Dropped client (illegal auth type: %d)\n", client_init_buffer[0]);
            goto client_abort;
        }

        printf("VNC_Init: Got good auth\n");

        // (status successful)
        if (SendAll(server->peer, "\x00\x00\x00\x00", 4))
        {
            printf("VNC_Init: Dropped client (could not send auth success)\n");
            goto client_abort;
        }

        // The shared flag doesn't really matter since we support only one
        // client anyway, just read it and stuff the value into some storage
        // we already have on hand
        if (RecvAll(server->peer, client_init_buffer, 1))
        {
            printf("VNC_Init: Dropped client (could not recv client init)\n");
            goto client_abort;
        }

        printf("VNC_Init: Got client init\n");

        byte server_init[28];
        server_init[0] = (server->width >> 8) & 0xff; // Framebuffer width
        server_init[1] = server->width & 0xff;
        server_init[2] = (server->height >> 8) & 0xff; // Framebuffer height
        server_init[3] = server->height & 0xff;
        server_init[4] = 32; // Bits per pixel
        server_init[5] = 24; // Depth
        server_init[6] = 0; // Big-endian flag
        server_init[7] = 1; // True color flag
        server_init[8] = 0; // Red scale
        server_init[9] = 255;
        server_init[10] = 0; // Green scale
        server_init[11] = 255;
        server_init[12] = 0; // Blue scale
        server_init[13] = 255;
        server_init[14] = 16; // Red shift
        server_init[15] = 8; // Green shift
        server_init[16] = 0; // Blue shift
        server_init[20] = 0; // Desktop name length
        server_init[21] = 0;
        server_init[22] = 0;
        server_init[23] = 4;
        server_init[24] = 'D'; // Desktop name
        server_init[25] = 'O';
        server_init[26] = 'O';
        server_init[27] = 'M';

        // At this point we've completed the handshake and have a working
        // connection with the client. It may send some more initial
        // configuration later but for the time being we can let the game continue.
        if (SendAll(server->peer, server_init, 28))
        {
            printf("VNC_Init: Dropped client (could not send server init)\n");
            goto client_abort;
        }

        // We can't accept any clients beyond the first, so don't bother listening
        close(server_sock);
        printf("VNC_Init: All done here, starting up with default raw encoding\n");
        break;

client_abort:
        close(server->peer);
        server->peer = -1;
        continue;
    }
}

void VNC_SetTextInput(vnc_server_t* server, boolean state)
{
    server->text_input = state;
}

void VNC_PumpMessages(vnc_server_t* server)
{
    fd_set readers = {0};

    // Return immediately, only pull the data that's buffered
    int events = 0;

    // Collapse several cursor movement packets into a single event
    int new_mouse_x = -1;
    int new_mouse_y = -1;
    int mouse_buttons = -1;
    do {
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        FD_ZERO(&readers);
        FD_SET(server->peer, &readers);

        events = select(server->peer + 1, &readers, NULL, NULL, &timeout);
        if (events == -1)
        {
            printf("VNC_PumpMessages: Could not poll (%s)\n", strerror(errno));
            return;
        }

        if (events == 1)
        {
            int chunk = recv(server->peer, server->client_packet + server->packet_cursor, VNC_PACKET_SIZE - server->packet_cursor, 0);
            if (chunk <= 0)
            {
                // If the client is dead here, then we have to abort. Otherwise
                // we could end up waiting here for a while which could throw
                // off the game's timing once somebody reconnects.
                printf("VNC_PumpMessages: socket read failure\n");
                VNC_Exit(server);
                I_Quit();
                break;
            }

            server->packet_cursor += chunk;

            int old_scan_pos = 0;
            int message_scan_pos = 0;
            while (message_scan_pos >= 0)
            {
              old_scan_pos = message_scan_pos;
              message_scan_pos = HandleVNCMessage(server, message_scan_pos, &new_mouse_x, &new_mouse_y, &mouse_buttons);
            }

            if (message_scan_pos == -2)
            {
                printf("VNC_PumpMessages: Flushing buffer after unknown message\n");
            }
            else
            {
                FinalizeVNCMessages(server, old_scan_pos);
            }
        }
    } while (events == 1);

    if (new_mouse_x != -1)
    {
        event_t event;
        event.type = ev_mouse;
        event.data1 = mouse_buttons;
        event.data2 = new_mouse_x - server->mouse_x;
        event.data3 = new_mouse_y - server->mouse_y;
        D_PostEvent(&event);

        server->mouse_x = new_mouse_x;
        server->mouse_y = new_mouse_y;
    }
}

void VNC_PreparePalette(vnc_server_t* server, rgb_t* palette)
{
    if (server->palette == NULL)
    {
        server->palette = malloc(server->width * server->height * 3);
    }

    // The palette is usually function scoped and loaded from a cached lump, so
    // it's not guaranteed to be around later on. Make sure we copy it.
    int offset = 0;
    for (int i = 0; i < 256; i++)
    {
        server->palette[offset++] = palette[i].r;
        server->palette[offset++] = palette[i].g;
        server->palette[offset++] = palette[i].b;
    }
}

static void SendRawVNCFrame(vnc_server_t* server, byte* frame)
{
    int offset = 0;
    server->server_packet[offset++] = VNC_SERVER_FRAMEBUFFERUPDATE;
    offset++; // Padding
    server->server_packet[offset++] = 0; // Number of rectangles worth of pixel data, we only ever send the whole screen
    server->server_packet[offset++] = 1;
    server->server_packet[offset++] = 0; // X coordinate
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = 0; // Y coordinate
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = (server->width >> 8); // Width
    server->server_packet[offset++] = server->width & 0xff;
    server->server_packet[offset++] = (server->height >> 8); // Height
    server->server_packet[offset++] = server->height & 0xff;
    server->server_packet[offset++] = 0; // Raw encoding type
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = VNC_RAW;
    if (SendAll(server->peer, server->server_packet, offset))
    {
        VNC_Exit(server);
        I_Quit();
        return;
    }

    offset = 0;
    for (int i = 0; i < server->width * server->height; i++)
    {
        server->server_packet[offset++] = server->palette[frame[i] * 3 + 2];
        server->server_packet[offset++] = server->palette[frame[i] * 3 + 1];
        server->server_packet[offset++] = server->palette[frame[i] * 3];
        server->server_packet[offset++] = 0;
    }

    if (SendAll(server->peer, server->server_packet, offset))
    {
        printf("SendRawVNCFrame: framebuffer send failure");
        VNC_Exit(server);
        I_Quit();
        return;
    }
}

static void SendTightVNCFrame(vnc_server_t* server, byte* frame)
{
    int offset = 0;
    server->server_packet[offset++] = VNC_SERVER_FRAMEBUFFERUPDATE;
    offset++; // Padding
    server->server_packet[offset++] = 0; // Number of rectangles worth of pixel data, we only ever send the whole screen
    server->server_packet[offset++] = 1;
    server->server_packet[offset++] = 0; // X coordinate
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = 0; // Y coordinate
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = (server->width >> 8); // Width
    server->server_packet[offset++] = server->width & 0xff;
    server->server_packet[offset++] = (server->height >> 8); // Height
    server->server_packet[offset++] = server->height & 0xff;
    server->server_packet[offset++] = 0; // Tight encoding type
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = 0;
    server->server_packet[offset++] = VNC_TIGHT;
    if (SendAll(server->peer, server->server_packet, offset))
    {
        VNC_Exit(server);
        I_Quit();
        return;
    }

    offset = 0;

    // Tight encoding is a compressed encoding that supports various options,
    // including JPEG encoding, palettes and gradients. The only thing we care
    // about here is palette support; Tight encoding supports palettes of 256
    // colors which is exactly the size we need to use the frame data with no
    // modification. See github.com/rfbproto/rfbproto for documentation.
    // RFC 6143 doesn't describe this encoding.
    //
    // Tight is also a zlib-compressed encoding, which is less useful because
    // I don't want to add zlib as a dependency. We can avoid actually having
    // to use zlib by abusing a quirk of the RFC 1951 format: it supports
    // blocks of data that are not compressed, and sets no limit on how they're
    // used.
    //
    // Without compression the main complexity here is framing things. We have
    // to use the RFC 1950 framing at the top-level, which looks like this:
    //
    // byte    0     1    ...        n+1 n+2 n+3 n+4
    //      | CMF | FLG | <data>  |      ADLER32
    //
    // Where our CMF is going to be (01111000 - DEFLATE with 32K window),
    // our FLG is going to be (0000001 - no dictionary, lowest compression level,
    // and some extra juice to get the right flag check value) and the ADLER32
    // checksum which is computed as follows over the frame data and stored in
    // big-endian byte order:
    //
    //   s2 = (s2 + byte[i]) % 65521
    //   s1 = (s1 + s2) % 65521
    //   ...
    //   ADLER32 = s2 << 16 | s1
    //
    // <data> is also composed of uncompressed DEFLATE blocks, which look like
    // this:
    //        0        1 2      3 4
    //   | 00000000 | LENGTH | NLENGTH | <framedata>
    //
    // The first byte stores the block header (here, it's not the last block and
    // has type 0), then the length (big-endian I assume) and the bitwise negation
    // of the length.
    //
    // That continues until the last block, which has its first bit set to indicate
    // that it's the last one in the stream.
    //
    // The Tight protocol itself is reasonably simple if you ignore the need to
    // compress it. We can fit the whole rectangle into a single Tight block since
    // we're never transmitting anything close to its limit (2048x2048).

    // We don't care about compression control, just reset stream 0 and always use
    // stream 0. Also, always use basic compression which contains pixel data.
    server->server_packet[offset++] = (1 << 6) | 1;

    // Configure the palette filter and send the palette data
    server->server_packet[offset++] = 1;

    // 256 total colors, followed by the colors themselves
    server->server_packet[offset++] = 255;
    int palette_pos = 0;
    for (int i = 0; i < 256; i++)
    {
        // Note that these are not endian-adjusted like in raw encoding, Tight
        // specifies that RGB is packed in this order
        server->server_packet[offset++] = server->palette[palette_pos++]; // Red
        server->server_packet[offset++] = server->palette[palette_pos++]; // Green
        server->server_packet[offset++] = server->palette[palette_pos++]; // Blue
    }

    // Figure out how much data we're going to need to emulate the ZLIB stream, and
    // then pack that into the Tight compact representation. We know that we'll need
    // 6 bytes for the ZLIB framing, and then 5 bytes for each 64K block of palette
    // indices (rounding up)
    //
    // Ideally we could just checkpoint the output here and come back to it, but we
    // can't because the size itself is variable width and we have to know how many
    // bytes it would take up in order to skip it
    int zlib_data_size = 6;
    int frame_data_size = server->width * server->height;
    int zlib_frames = (frame_data_size >> 16) + 1;

    zlib_data_size += frame_data_size + 5 * zlib_frames;
    if (zlib_data_size < 0x80)
    {
        server->server_packet[offset++] = zlib_data_size;
    }
    else if (zlib_data_size < 0x4000)
    {
        server->server_packet[offset++] = (1 << 7) | (zlib_data_size & 0x7f);
        server->server_packet[offset++] = (zlib_data_size >> 7) & 0x7f;
    }
    else
    {
        server->server_packet[offset++] = (1 << 7) | (zlib_data_size & 0x7f);
        server->server_packet[offset++] = (1 << 7) | ((zlib_data_size >> 7) & 0x7f);
        server->server_packet[offset++] = (zlib_data_size >> 14) & 0xff;
    }

    int zlib_data_start = offset;

    // Finally, write out our mock zlib data and compute the checksum as we do
    int adler_s1 = 1;
    int adler_s2 = 0;
    // DEFLATE compression method with a window size of 9 (?)
    server->server_packet[offset++] = (1 << 6) | (1 << 5) | (1 << 4) | (1 << 3);
    // Check bits along with no dictionary and low compression level
    server->server_packet[offset++] = 1;

    int data_left = frame_data_size;
    int zlib_block_capacity = 0;
    for (int i = 0; i < frame_data_size; i++)
    {
        if (zlib_block_capacity == 0)
        {
            zlib_block_capacity = 0xffff;
            if (data_left <= 0xffff)
            {
                int data_left_hi = (data_left >> 8) & 0xff;
                int data_left_lo = data_left & 0xff;
                server->server_packet[offset++] = 1;
                server->server_packet[offset++] = data_left_lo;
                server->server_packet[offset++] = data_left_hi;
                server->server_packet[offset++] = ~data_left_lo;
                server->server_packet[offset++] = ~data_left_hi;
            }
            else
            {
                server->server_packet[offset++] = 0;
                server->server_packet[offset++] = 0xff;
                server->server_packet[offset++] = 0xff;
                server->server_packet[offset++] = 0;
                server->server_packet[offset++] = 0;
            }
        }

        adler_s1 += frame[i];
        if (adler_s1 >= 65521)
        {
            adler_s1 -= 65521;
        }

        adler_s2 += adler_s1;
        if (adler_s2 >= 65521)
        {
            adler_s2 -= 65521;
        }

        server->server_packet[offset++] = frame[i];
        zlib_block_capacity--;
        data_left--;
    }

    server->server_packet[offset++] = (adler_s2 >> 8) & 0xff;
    server->server_packet[offset++] = adler_s2 & 0xff;
    server->server_packet[offset++] = (adler_s1 >> 8) & 0xff;
    server->server_packet[offset++] = adler_s1 & 0xff;

    if (SendAll(server->peer, server->server_packet, offset))
    {
        printf("SendTightVNCFrame: framebuffer send failure");
        VNC_Exit(server);
        I_Quit();
        return;
    }
}

void VNC_SendFrame(vnc_server_t* server, byte* frame)
{
    if (!server->send_frame)
    {
        return;
    }

    if (!server->palette)
    {
        printf("VNC_PumpMessages: Deferring send until palette is availble\n");
        return;
    }

    switch (server->encoding)
    {
        case VNC_RAW:
            SendRawVNCFrame(server, frame);
            break;
        case VNC_TIGHT:
            SendTightVNCFrame(server, frame);
            break;
    }

    server->send_frame = 0;
}

void VNC_Exit(vnc_server_t* server)
{
    if (server->palette != NULL)
    {
        free(server->palette);
        server->palette = NULL;
    }

    if (server->server_packet != NULL)
    {
        free(server->server_packet);
        server->server_packet = NULL;
    }

    close(server->peer);
}
