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

#include <stdio.h>
#include <glib.h>
#include <unistd.h>

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "remote/pcihost.h"
#include "remote/machine.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "qemu/main-loop.h"
#include "remote/memory.h"
#include "io/proxy-link.h"
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

static ProxyLinkState *proxy_link;
PCIDevice *remote_pci_dev;

static void process_msg(GIOCondition cond)
{
    ProcMsg *msg = NULL;
    Error *err = NULL;

    if ((cond & G_IO_HUP) || (cond & G_IO_ERR)) {
        error_setg(&err, "socket closed, cond is %d", cond);
        goto finalize_loop;
    }

    msg = g_malloc0(sizeof(ProcMsg));

    if (proxy_proc_recv(proxy_link, msg) < 0) {
        error_setg(&err, "Failed to receive message");
        goto finalize_loop;
    }

    switch (msg->cmd) {
    case INIT:
        break;
    case CONF_WRITE:
        break;
    case CONF_READ:
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
    proxy_link_finalize(proxy_link);
    proxy_link = NULL;
}

static void *remote_main_loop(void *data)
{
    while (1) {
        qemu_mutex_lock_iothread();
        main_loop_wait(false);
        qemu_mutex_unlock_iothread();
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    Error *err = NULL;
    QemuThread main_loop_thread;

    module_call_init(MODULE_INIT_QOM);

    bdrv_init_with_whitelist();

    if (qemu_init_main_loop(&err)) {
        error_report_err(err);
        return -EBUSY;
    }

    qemu_init_cpu_loop();

    page_size_init();

    current_machine = MACHINE(REMOTE_MACHINE(object_new(TYPE_REMOTE_MACHINE)));

    proxy_link = proxy_link_create();
    if (!proxy_link) {
        printf("Could not create proxy link\n");
        return -1;
    }

    proxy_link_set_sock(proxy_link, STDIN_FILENO);
    proxy_link_set_callback(proxy_link, process_msg);

    qemu_thread_create(&main_loop_thread, "remote-main-loop", remote_main_loop,
                       NULL, QEMU_THREAD_DETACHED);

    start_handler(proxy_link);

    qemu_thread_cancel(&main_loop_thread);

    return 0;
}
