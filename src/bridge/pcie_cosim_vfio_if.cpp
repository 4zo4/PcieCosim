/*
 * Copyright (c) 2019, Nutanix Inc. All rights reserved.
 *     Author: Thanos Makatos <thanos@nutanix.com>
 *             Swapnil Ingle <swapnil.ingle@nutanix.com>
 *             Felipe Franciosi <felipe@nutanix.com>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *      * Neither the name of Nutanix nor the names of its contributors may be
 *        used to endorse or promote products derived from this software without
 *        specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 *
 * Original code from: https://github.com/nutanix/libvfio-user/blob/master/samples/gpio-pci-idio-16.c 
 * Modified by Purple, 2026
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace std::literals;

#ifndef _Static_assert
#define _Static_assert static_assert
#endif

#include "common.h"
#include "libvfio-user.h"

#define PCI_VENDOR_ID_VFIO 0x494f
#define PCI_DEVICE_ID_VFIO 0x0dc8

#ifndef VFU_CAP_FLAG_NONE
#define VFU_CAP_FLAG_NONE 0
#endif
#define VFIO_PCIE_SOCK "/tmp/vfio-pcie.sock"

ssize_t pcie_bridge_bar0(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write);
ssize_t pcie_bridge_bar1(vfu_ctx_t *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write);
int pcie_bridge_init(void);
int pcie_bridge_disconnect(void);
int test_pcie_bridge();
void pcie_bridge_terminate_monitor();
void pcie_bridge_async_listener(vfu_ctx_t *vfu_ctx);

static int terminate_req[2] = { -1, -1 };

static void
_log(vfu_ctx_t *vfu_ctx UNUSED, UNUSED int level, char const *msg)
{
    fprintf(stderr, "vfio: %s\n", msg);
}

static void _sa_handler(UNUSED int signum)
{
    if (terminate_req[1] != -1) {
        char token = 'X';
        [[maybe_unused]] ssize_t bytes_written = write(terminate_req[1], &token, 1);
    }
}

static void
dma_register(UNUSED vfu_ctx_t *vfu_ctx, UNUSED vfu_dma_info_t *info)
{
}

static void
dma_unregister(UNUSED vfu_ctx_t *vfu_ctx, UNUSED vfu_dma_info_t *info)
{
}

int
main(int argc, char *argv[])
{
    int ret;
    bool verbose = false;
    bool restart = true;
    struct sigaction act = {0};
    act.sa_handler = _sa_handler;
    vfu_ctx_t *vfu_ctx;
    int opt;

     while ((opt = getopt(argc, argv, "Rv")) != -1) {
        switch (opt) {
            case 'R':
                restart = false;
                break;
            case 'v':
                verbose = true;
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s [-Rv]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    while ((opt = getopt(argc, argv, "Rv")) != -1) {
        switch (opt) {
            case 'R':
                restart = false;
                break;
            case 'v':
                verbose = true;
                break;
            default: /* '?' */
                fprintf(stderr, "Usage: %s [-Rv]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);

    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        perror("failed to block SIGINT on main thread");
        exit(EXIT_FAILURE);
    }

    if (pipe(terminate_req) == -1) {
        perror("failed to create termination pipe");
        exit(EXIT_FAILURE);
    }

    std::thread term_thread(pcie_bridge_terminate_monitor);
    term_thread.detach();

    sigemptyset(&act.sa_mask);
    if (sigaction(SIGINT, &act, NULL) == -1) {
        err(EXIT_FAILURE, "failed to register signal handler");
    }

    vfu_ctx = vfu_create_ctx(VFU_TRANS_SOCK, VFIO_PCIE_SOCK, 0, NULL,
                             VFU_DEV_TYPE_PCI);
    if (vfu_ctx == NULL) {
        if (errno == EINTR) {
            printf("interrupted\n");
            exit(EXIT_SUCCESS);
        }
        err(EXIT_FAILURE, "failed to initialize device emulation");
    }

    ret = vfu_setup_log(vfu_ctx, _log, verbose ? LOG_DEBUG : LOG_ERR);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup log");
    }

    ret = vfu_pci_init(vfu_ctx, VFU_PCI_TYPE_CONVENTIONAL,
                       PCI_HEADER_TYPE_NORMAL, 0);
    if (ret < 0) {
        err(EXIT_FAILURE, "vfu_pci_init() failed");
    }

    vfu_pci_set_id(vfu_ctx, PCI_VENDOR_ID_VFIO, PCI_DEVICE_ID_VFIO, 0x0, 0x0);

    ret = pcie_bridge_init();

    if (ret < 0) {
        err(EXIT_FAILURE, "failed to initialize PCIe bridge");
    }

    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR0_REGION_IDX,
                           0x1000,      // 4KB size
                           pcie_bridge_bar0, // The IPC-triggering callback
                           VFU_REGION_FLAG_RW | VFU_REGION_FLAG_ALWAYS_CB,
                           NULL, 0,     // NO MMAP (Force intercept)
                           -1, 0);      // No direct FD
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR0 region");
    }

    ret = vfu_setup_region(vfu_ctx, VFU_PCI_DEV_BAR1_REGION_IDX,
                           0x1000,      // 4KB size
                           pcie_bridge_bar1, // The IPC-triggering callback
                           VFU_REGION_FLAG_RW | VFU_REGION_FLAG_ALWAYS_CB | VFU_REGION_FLAG_MEM,
                           NULL, 0,     // NO MMAP (Force intercept)
                           -1, 0);      // No direct FD
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup BAR1 region");
    }

    ret = vfu_setup_device_nr_irqs(vfu_ctx, VFU_DEV_MSI_IRQ, 1);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup MSI irq counts");
    }

    // Define a compliant 64-bit MSI Capability Structure layout
    // Byte 0: Capability ID (0x05 for MSI)
    // Byte 1: Next Capability Pointer (0x00 indicating end of list)
    // Byte 2-3: Message Control Register (0x0081: 64-bit address capable, 1 vector requested, MSI enabled state tracking)
    struct msi_cap_hdr {
        uint8_t id;
        uint8_t next;
        uint16_t msg_ctrl;
        uint32_t msg_addr_lo;
        uint32_t msg_addr_hi;
        uint16_t msg_data;
        uint16_t padding;
    } __attribute__((packed));

    msi_cap_hdr msi_cap = {};
    msi_cap.id = 0x05;         // PCI MSI Identifier
    msi_cap.next = 0x00;       // End of the link chain tracking structures
    msi_cap.msg_ctrl = 0x0081; // Bit 0 = 1 (MSI Enable tracking support), Bit 7 = 1 (64-bit capable)

    ret = vfu_pci_add_capability(vfu_ctx, 0,
                                 VFU_CAP_FLAG_NONE,   // Default access rules
                                 (uint8_t*)&msi_cap); // Pointer to MSI Capability structure 
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to add MSI capability to configuration space");
    }

    ret = vfu_setup_device_dma(vfu_ctx, LIBVFIO_USER_MAX_DMA_REGIONS,
                               dma_register, dma_unregister);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to setup DMA");
    }

    std::thread irq_thread(pcie_bridge_async_listener, vfu_ctx);
    irq_thread.detach();

    ret = vfu_realize_ctx(vfu_ctx);
    if (ret < 0) {
        err(EXIT_FAILURE, "failed to realize device");
    }
#if defined(ENABLE_TEST) && (ENABLE_TEST == 1)
    if (restart) {
        printf("Testing PCIe bridge\n");

        ret = test_pcie_bridge();
        {
            constexpr auto msg = "[PCIe-Bridge] Terminated\n"sv; 
            write(STDOUT_FILENO, msg.data(), msg.size());
        }
        unlink(VFIO_PCIE_SOCK);
        _exit(EXIT_SUCCESS);
    }
#else // Normal PCIe Bridge execution: attach to VFIO and run
    ret = vfu_attach_ctx(vfu_ctx);
    if (ret < 0) {
        int _errno = errno;
        vfu_destroy_ctx(vfu_ctx);
        errno = _errno;
        err(EXIT_FAILURE, "failed to attach device");
    }
#endif
    do {
        ret = vfu_run_ctx(vfu_ctx);
        if (ret != 0) {
            int _errno = errno;
            if (_errno == ENOTCONN) {
                ret = vfu_attach_ctx(vfu_ctx);
                if (ret < 0) {
                    err(EXIT_FAILURE, "failed to re-attach device");
                 }
            } else if (_errno != EINTR) {
                err(EXIT_FAILURE, "vfu_run_ctx() failed");
            }
        }
    } while (restart);

    vfu_destroy_ctx(vfu_ctx);
    return EXIT_SUCCESS;
}

void pcie_bridge_terminate_monitor()
{
    char token;
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);

    if (pthread_sigmask(SIG_UNBLOCK, &set, NULL) != 0) {
        perror("failed to unblock SIGINT on monitor thread");
        _exit(EXIT_FAILURE);
    }

    if (terminate_req[0] != -1) {
        [[maybe_unused]] ssize_t bytes_read = read(terminate_req[0], &token, 1);
    }

    {
        constexpr auto msg = "[PCIe-Bridge] received SIGINT. Terminating...\n"sv; 
        write(STDOUT_FILENO, msg.data(), msg.size()); 
    }

    pcie_bridge_disconnect();

    {
        constexpr auto msg = "[PCIe-Bridge] Terminated\n"sv; 
        write(STDOUT_FILENO, msg.data(), msg.size());
    }

    unlink(VFIO_PCIE_SOCK);
    _exit(EXIT_SUCCESS);
}

/* ex: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
