/*
 * Remote device initialization
 *
 * Copyright 2019, Oracle and/or its affiliates.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"

#include <stdio.h>
#include <unistd.h>

#include "qemu/module.h"
#include "remote/pcihost.h"
#include "remote/machine.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "qemu/main-loop.h"
#include "remote/memory.h"
#include "io/mpqemu-link.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "qemu-common.h"
#include "hw/pci/pci.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/config-file.h"
#include "sysemu/sysemu.h"
#include "block/block.h"
#include "exec/memattrs.h"

static MPQemuLinkState *mpqemu_link;
PCIDevice *remote_pci_dev;

static void process_config_write(MPQemuMsg *msg)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;

    qemu_mutex_lock_iothread();
    pci_default_write_config(remote_pci_dev, conf->addr, conf->val, conf->l);
    qemu_mutex_unlock_iothread();
}

static void process_config_read(MPQemuMsg *msg)
{
    struct conf_data_msg *conf = (struct conf_data_msg *)msg->data2;
    uint32_t val;
    int wait;

    wait = msg->fds[0];

    qemu_mutex_lock_iothread();
    val = pci_default_read_config(remote_pci_dev, conf->addr, conf->l);
    qemu_mutex_unlock_iothread();

    notify_proxy(wait, val);

    PUT_REMOTE_WAIT(wait);
}

/* TODO: confirm memtx attrs. */
static void process_bar_write(MPQemuMsg *msg, Error **errp)
{
    bar_access_msg_t *bar_access = &msg->data1.bar_access;
    AddressSpace *as =
        bar_access->memory ? &address_space_memory : &address_space_io;
    MemTxResult res;

    res = address_space_rw(as, bar_access->addr, MEMTXATTRS_UNSPECIFIED,
                           (uint8_t *)&bar_access->val, bar_access->size, true);

    if (res != MEMTX_OK) {
        error_setg(errp, "Could not perform address space write operation,"
                   " inaccessible address: %lx.", bar_access->addr);
    }
}

static void process_bar_read(MPQemuMsg *msg, Error **errp)
{
    bar_access_msg_t *bar_access = &msg->data1.bar_access;
    AddressSpace *as;
    int wait = msg->fds[0];
    MemTxResult res;
    uint64_t val = 0;

    as = bar_access->memory ? &address_space_memory : &address_space_io;

    assert(bar_access->size <= sizeof(uint64_t));

    res = address_space_rw(as, bar_access->addr, MEMTXATTRS_UNSPECIFIED,
                           (uint8_t *)&val, bar_access->size, false);

    if (res != MEMTX_OK) {
        error_setg(errp, "Could not perform address space read operation,"
                   " inaccessible address: %lx.", bar_access->addr);
        val = (uint64_t)-1;
        goto fail;
    }

    switch (bar_access->size) {
    case 4:
        val = *((uint32_t *)&val);
        break;
    case 2:
        val = *((uint16_t *)&val);
        break;
    case 1:
        val = *((uint8_t *)&val);
        break;
    default:
        error_setg(errp, "Invalid PCI BAR read size");
        return;
    }

fail:
    notify_proxy(wait, val);

    PUT_REMOTE_WAIT(wait);
}

static void process_msg(GIOCondition cond, MPQemuChannel *chan)
{
    MPQemuMsg *msg = NULL;
    Error *err = NULL;

    if ((cond & G_IO_HUP) || (cond & G_IO_ERR)) {
        error_setg(&err, "socket closed, cond is %d", cond);
        goto finalize_loop;
    }

    msg = g_malloc0(sizeof(MPQemuMsg));

    if (mpqemu_msg_recv(mpqemu_link, msg, chan) < 0) {
        error_setg(&err, "Failed to receive message");
        goto finalize_loop;
    }

    switch (msg->cmd) {
    case INIT:
        break;
    case CONF_WRITE:
        process_config_write(msg);
        break;
    case CONF_READ:
        process_config_read(msg);
        break;
    case BAR_WRITE:
        process_bar_write(msg, &err);
        if (err) {
            goto finalize_loop;
        }
        break;
    case BAR_READ:
        process_bar_read(msg, &err);
        if (err) {
            goto finalize_loop;
        }
        break;
    default:
        error_setg(&err, "Unknown command");
        goto finalize_loop;
    }

    g_free(msg);

    return;

finalize_loop:
    error_report_err(err);
    g_free(msg);
    mpqemu_link_finalize(mpqemu_link);
    mpqemu_link = NULL;
}

int main(int argc, char *argv[])
{
    Error *err = NULL;

    module_call_init(MODULE_INIT_QOM);

    bdrv_init_with_whitelist();

    if (qemu_init_main_loop(&err)) {
        error_report_err(err);
        return -EBUSY;
    }

    qemu_init_cpu_loop();

    page_size_init();

    current_machine = MACHINE(REMOTE_MACHINE(object_new(TYPE_REMOTE_MACHINE)));

    mpqemu_link = mpqemu_link_create();
    if (!mpqemu_link) {
        printf("Could not create MPQemu link\n");
        return -1;
    }

    mpqemu_init_channel(mpqemu_link, &mpqemu_link->com, STDIN_FILENO);
    mpqemu_link_set_callback(mpqemu_link, process_msg);

    mpqemu_start_coms(mpqemu_link);

    return 0;
}
