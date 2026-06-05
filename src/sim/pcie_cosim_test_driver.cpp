
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <string>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "protocol.h"
#include "socket_channel.h"

ssize_t pcie_bridge_bar0(struct vfu_ctx *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write);
ssize_t pcie_bridge_bar1(struct vfu_ctx *vfu_ctx, char *buf, size_t count, loff_t offset, bool is_write);
int pcie_bridge_disconnect(void);

#define BAR0 0
#define BAR1 1
#define BAR_ID BAR0

#define CONCAT2(a, b) a##b
#define CONCAT(a, b) CONCAT2(a, b)

#define pcie_bridge_bar(id, buf, count, offset, is_write) \
    CONCAT(pcie_bridge_bar, id)(nullptr, (char *)(buf), count, offset, is_write)

int test_pcie_bridge()
{
    ssize_t ret = 0;
    uint32_t wbuf[16] = {0x04030201, 0x08070605, 0x01020304, 0x05060708,
                         0x11111111, 0x22222222, 0x33333333, 0x44444444,
                         0x55555555, 0x66666666, 0x77777777, 0x88888888,
                         0x99999999, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC};
    uint32_t rbuf[16] = {0};
    size_t count = 16 * sizeof(uint32_t);
    loff_t offset = 0x04;
    uint32_t trigger_irq = 0xACDCBABE;
    int k = 0, m = 0;

    char match[512] = "Data match at index";
    char mismatch[512] = "Data mismatch at index";
    int matchIdx = strlen(match);
    int mismatchIdx = strlen(mismatch);

    ret = pcie_bridge_bar(BAR_ID, wbuf, count, offset, true);

    if (ret != static_cast<ssize_t>(count)) {
        ret = -1;
        goto out;
    }

    ret = pcie_bridge_bar(BAR_ID, rbuf, count, offset, false);

    if (ret != static_cast<ssize_t>(count)) {
        ret = -1;
        goto out;
    }

    for (size_t i = 0; i < count/sizeof(uint32_t); i++) {
        if (rbuf[i] != wbuf[i]) {
            if (m++ % 4 == 0) {
                mismatchIdx += sprintf(mismatch + mismatchIdx, "\n");
            }
            mismatchIdx += sprintf(mismatch + mismatchIdx, " %ld=(WR 0x%x, RD 0x%x)", i, (int)wbuf[i], (int)rbuf[i]);
            ret = -1;
        } else {
            if (k++ % 4 == 0) {
                matchIdx += sprintf(match + matchIdx, "\n");
            }
            matchIdx += sprintf(match + matchIdx, " %ld=(WR 0x%x, RD 0x%x)", i, (int)wbuf[i], (int)rbuf[i]);
        }
    }

    pcie_bridge_bar(BAR1, &trigger_irq, 4, 0, true);

    if (ret < 0) {
        std::cerr << mismatch << std::endl;
        printf("PCIe bridge test fail\n");
    } else {
        std::cout << match << std::endl;
        printf("PCIe bridge test pass\n");
    }
out:
    pcie_bridge_disconnect();
    return ret;
}