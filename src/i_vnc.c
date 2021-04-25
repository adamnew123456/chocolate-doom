#include "d_event.h"
#include "i_system.h"
#include "i_vnc.h"

#include <errno.h>
#include <string.h>
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

static const char[] vnc_keysym_unshifted = {
    // Control characters, these have no meaningful casing
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
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

static int ConsumeVNCMessage(vnc_server_t* server, int size)
{
    if (server->packet_cursor < size)
    {
        return 0;
    }

    if (server->packet_cursor == size)
    {
        server->packet_cursor = 0;
        return 1;
    }
 
    // *Hopefully* this is smart enough to figure out that it can safely copy
    // through the array backwards without having to use scratch space
    memmove(server->client_packet, server->client_packet + size, server->packet_cursor - size);
    return 1;
}

static int HandleVNCMessage(vnc_server_t *server, int* cursor_x, int* cursor_y, int* mouse_buttons)
{
    switch (server->client_packet[0])
    {
        case VNC_CLIENT_SETPIXELFORMAT:
            // We're hardcoded to using the palette pixel format, so the client has to deal with it
            if (ConsumeVNCMessage(server, 20))
            {
                printf("VNC_PumpMessage: Ignoring client pixel format request\n");
                return 1;
            }
            break;

        case VNC_CLIENT_SETENCODING:
            // Same for the encoding, we just use raw for the moment
            if (server->packet_cursor >= 4)
            {
                // We can figure out the true size by looking at the encoding count
                int encoding_count = (server->client_packet[2] << 8) | server->client_pakcet[3];
                int expect_length = 4 + encoding_count * 4;

                if (ConsumeVNCMessage(server, expect_length))
                {
                    printf("VNC_PumpMessage: Ignoring client encoding request\n");
                    return 1;
                }
            }
            break;

        case VNC_CLIENT_FRAMEBUFFERUPDATEREQUEST:
            if (ConsumeVNCMessage(server, 10))
            {
                printf("VNC_PumpMessage: Sending packet on next frame\n");
                server->send_frame = 1;
                return 1;
            }
            break;

        case VNC_CLIENT_KEYEVENT: 
            {
                byte is_keydown = server->client_packet[1];
                int keysym = 
                    (server->client_packet[4] << 24) |
                    (server->client_packet[5] << 16) |
                    (server->client_packet[6] << 8) |
                    server->client_packet[7];
                boolean is_key_known = keysym <= 0x7f;

                if (ConsumeVNCMessage(server, 10))
                {
                    event_t event;
                    event.data2 = 0;
                    event.date3 = 0;

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
                    printf("VNC_PumpMessage: Received key\n");

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
                            keysym = KEY_PRINTSCR;
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
                            keysym = KEY_PGDOWN;
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
                        return 1;
                    }

                    int keysym_lowercase = keysym;
                    if (keysym <= 0x7f && vnc_keysym_unshifted[keysym] != 0)
                    {
                        keysym_lowercase = vnc_keysym_unshifted[keysym];
                    }

                    if (keysym >= 0x41 && keysym < 0x5b)
                    {
                        keysym_lowercase += 0x20;
                    }

                    if (keysym >= 0x20 && keysym < 0x7f)
                    {
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
                        event_t event;
                        event.type = ev_quit;
                    }
                    
                    D_PostEvent(&event);
                    return 1;
                }

                break;
            }

        case VNC_CLIENT_POINTEREVENT:
            {
                int x_pos = (server->client_packet[2] << 8) | server->client_packet[3];
                int y_pos = (server->client_packet[4] << 8) | server->client_packet[5];
                int left_button = server->client_packet[1] & 0x1;
                int right_button = server->client_packet[1] & 0x4;
                int middle_button = server->client_packet[1] & 0x2;

                if (ConsumeVNCMessage(server, 6))
                {
                    *cursor_x = x_pos;
                    *cursor_y = y_pos;
                    *mouse_buttons = left_button | (right_button << 1) | (middle_button << 2);

                    // Defer this event so we can collect all mouse packets into a single event
                    return 1;
                }
                break;
            }

        case VNC_CLIENT_CLIENTCUTTEXT:
            // Length is dependent on data here, like SETENCODING
            if (server->packet_cursor >= 8)
            {
                // XXX: This could easily overfill our buffer if the user sends data longer than VNC_PACKET_SIZE
                // The best we can hope for is that data greater than that has no control characters in it, so
                // that we can drop the buffer repeatedly and then resync at some later point
                int encoding_count = 
                    (server->client_packet[4] << 24) | 
                    (server->client_packet[5] << 16) | 
                    (server->client_packet[6] << 8) | 
                    server->client_pakcet[7];

                int expect_length = 8 + encoding_count;
                if (ConsumeVNCMessage(server, expect_length))
                {
                    printf("VNC_PumpMessage: Ignoring client clipboard data\n");
                    return 1;
                }
            }

        default:
            // We couldn't understand the message, so just drop the buffer and
            // hopefully we'll resync at some point
            server->packet_cursor = 0;
            break;
    }

    return 0;
}

void VNC_Init(vnc_server_t* server, int width, int height)
{
    server->send_frame = false;
    server->text_input = false;
    server->palette = NULL;
    server->mouse_x = 0;
    server->mouse_y = 0;
    server->width = width;
    server->height = height;

    // Wait for a client to show up. Make sure we say something so that the
    // user knows we're waiting for something and not just idling.
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in listen_addr;
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = 1666;
    listen_addr.sin_addr = 0;

    struct sockaddr_in peer_addr;
    struct socklen_t peer_addrlen;

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
        byte[12] client_init_buffer;
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
            SendAll(server->peer, "\x00\x00\x00\x00\x13Unsupported version"
            printf("VNC_Init: Dropped client (invalid verstr: '%12.s')\n");
            goto client_abort;
        }

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

        if (client_sectype != 1)
        {
            // The client chose an illegal auth type, somehow
            // (status failed; 17-byte reason string)
            SendAll(server->peer, "\x00\x00\x00\x01\x00\x00\x00\x11Illegal auth type"
            printf("VNC_Init: Dropped client (illegal auth type: %d)\n", client_sectype);
            goto client_abort;
        }

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

        byte[28] server_init;
        server_init[0] = (server->width >> 8) & 0xff; // Framebuffer width
        server_init[1] = server->width & 0xff;
        server_init[2] = (server->height >> 8) & 0xff; // Framebuffer height
        server_init[3] = server->height & 0xff;
        server_init[4] = 8; // Bits per pixel
        server_init[5] = 8; // Depth
        server_init[6] = 8; // Big-endian flag
        server_init[7] = 8; // True color flag
        // The rest of the pixel format is irrelevant since we're using palettes
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
    fd_set readers, writers, errorers;

    // Return immediately, only pull the data that's buffered
    int events = 0;
    do {
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        FD_ZERO(&readers);
        FD_ZERO(&writers);
        FD_ZERO(&errorers);
        FD_SET(server->peer, &readers);

        events = select(1, &readers, &writers, &errorers, &timeout);
        if (events == -1)
        {
            printf("VNC_PumpMessages: Could not poll (%s)\n", strerror(errno));
            return;
        }

        // Collapse several cursor movement packets into a single event
        int new_mouse_x = -1;
        int new_mouse_y = -1;
        int mouse_buttons = -1;

        if (events == 1)
        {
            int chunk = recv(server->peer, server->client_packet + server->packet_cursor, VNC_PACKET_SIZE - server->packet_cursor);
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

            int exhausted = 0;
            server->packet_cursor += chunk;
            while (server->packet_cursor > 0)
            {
                if (!HandleVNCMessage(server, &new_mouse_x, &new_mouse_y, &mouse_buttons))
                {
                    break;
                }
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

void VNC_PreparePalette(vnc_server_t* server, byte* palette)
{
    server->palette = palette;
}

void VNC_SendFrame(vnc_server_t* server, byte* frame)
{
    if (!server->send_frame)
    {
        return;
    }

    int offset = 0;
    if (server->palette)
    {
        server->server_packet[offset++] = VNC_SERVER_SETCOLORMAPENTRIES;
        offset++; // Padding
        server->server_packet[offset+=] = 0; // Start updating the palette at offset 0 
        server->server_packet[offset++] = 0;
        server->server_packet[offset++] = 0;  // 255 total palette entries to write
        server->server_packet[offset++] = 255;

        int offset = 0;
        for (int i = 0; i < 256; i++)
        {
            // VNC palettes are scaled from 0-65535 instead of 0-255, so we have to scale correct them
            // by shifting the original RGB value from the lowest byte into the highest byte
            server->server_packet[offset++] = *(server->palette++);
            server->server_packet[offset++] = 0;
            server->server_packet[offset++] = *(server->palette++);
            server->server_packet[offset++] = 0;
            server->server_packet[offset++] = *(server->palette++);
            server->server_packet[offset++] = 0;
        }

        if (SendAll(server->peer, server->server_packet, offset))
        {
            printf("VNC_SendFrame: palette send failure");
            VNC_Exit(server);
            I_Quit();
            break;
        }

        server->palette = NULL;
        offset = 0;
    }

    int frame_size = server->width * server->height; // Each cell in the frame is 1 byte wide due to palettes
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
    if (SendAll(server->peer, server->server_packet, offset))
    {
        VNC_Exit(server);
        I_Quit();
        break;
    }

    if (SendAll(server->peer, frame, offset))
    {
        printf("VNC_SendFrame: framebuffer send failure");
        VNC_Exit(server);
        I_Quit();
        break;
    }

    server->send_frame = 0;
}

void VNC_Exit(vnc_server_t* server)
{
    close(server->peer);
}
