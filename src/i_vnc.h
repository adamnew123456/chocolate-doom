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
// DESCRIPTION:
//	Networked graphics via VNC (RFC 6143)
//

#ifndef __I_VNC__
#define __I_VNC__

#define VNC_PACKET_SIZE 1024
#define VNC_FRAME_SIZE 2048

#include "doomtype.h"

typedef struct {
    // The file descriptor we actually send network data on, comes from
    // accept() on server socket. Once this connection closes we kill the 
    // game.
    int peer;

    // Whether the client has sent a framebuffer update request that we need to
    // honor when the next frame is drawn.
    boolean send_frame;

    // The buffer that we use for storing server packets. Very large since its
    // primary purpose is storing pixel data which is encoded in raw format.
    byte server_packet[VNC_FRAME_SIZE];

    // The buffer that we use for receiving packets over the connection. This is
    // used to contain commands that we have received in part but have not gotten
    // all the information we need to fully process.
    byte client_packet[VNC_PACKET_SIZE];

    // The position in the client packet buffer that we can start writing the next
    // blob of partial packet data to.
    int packet_cursor;

    // Whether the user is currently in text input. Affects how we translate VNC key
    // events into game key events
    boolean text_input;

    // The palette to be pushed over with the next frame, 256*3 bytes
    byte *palette;

    // The last recorded positions of the mouse. Required since mouse events are relative.
    int mouse_x, mouse_y;

    // The width and height of the framebuffer we're sending data over
    int width, height;
} vnc_server_t;

// Waits for a client to connect and perform the initial handshake, then fills in the
// connection data within the server state record. 
void VNC_Init(vnc_server_t* server, int width, int height);

// Reads all the pending messages on the socket and processes them. This will fill
// the client_packet with any leftover data that was not part of a complete packet.
void VNC_PumpMessages(vnc_server_t* server);

// Saves the current palette to be sent over before the next frame.
void VNC_PreparePalette(vnc_server_t* server, byte* palette);

// Sends over the current frame of video data, if the client has requested it.
void VNC_SendFrame(vnc_server_t* server, byte* frame);

// Terminates the VNC connection. Usually this will be handled internally when the
// client hangs up on us, but if the user goes through the trouble to hit "Exit Game"
// then the cleanup also happens externally here.
void VNC_Exit(vnc_server_t* server);

#endif
