/*
 * Communication channel between QEMU and remote device process
 *
 * Copyright © 2018, 2020 Oracle and/or its affiliates.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef MPQEMU_LINK_H
#define MPQEMU_LINK_H

#include "qom/object.h"
#include "qemu/thread.h"
#include "io/channel.h"

#define REMOTE_MAX_FDS 8

#define MPQEMU_MSG_HDR_SIZE offsetof(MPQemuMsg, data.u64)

/**
 * MPQemuCmd:
 *
 * MPQemuCmd enum type to specify the command to be executed on the remote
 * device.
 */
typedef enum {
    INIT = 0,
    MAX = INT_MAX,
} MPQemuCmd;

/**
 * MPQemuMsg:
 * @cmd: The remote command
 * @size: Size of the data to be shared
 * @data: Structured data
 * @fds: File descriptors to be shared with remote device
 *
 * MPQemuMsg Format of the message sent to the remote device from QEMU.
 *
 */
typedef struct {
    int cmd;
    size_t size;

    union {
        uint64_t u64;
    } data;

    int fds[REMOTE_MAX_FDS];
    int num_fds;
} MPQemuMsg;

void mpqemu_msg_send(MPQemuMsg *msg, QIOChannel *ioc, Error **errp);
void mpqemu_msg_recv(MPQemuMsg *msg, QIOChannel *ioc, Error **errp);

bool mpqemu_msg_valid(MPQemuMsg *msg);
void mpqemu_msg_cleanup(MPQemuMsg *msg);

#endif
