#include "nvme.h"
#include "pci.h"
#include "paging.h"
#include "phys_mem.h"

enum {
    NVME_CLASS    = 0x01U,
    NVME_SUBCLASS = 0x08U,

    NVME_REG_DOORBELL_BASE = 0x1000U,

    NVME_CC_EN    = 1U << 0,
    NVME_CSTS_RDY = 1U << 0,

    NVME_OPC_IDENTIFY = 0x06U,

    NVME_CNS_NAMESPACE  = 0x00U,
    NVME_CNS_CONTROLLER = 0x01U,

    NVME_WAIT_SPINS = 10000000U,

    NVME_ADMIN_QUEUE_ENTRIES = 64U
};

#define NVME_MMIO_VADDR (KERNEL_OFFSET + 0x41003000ULL)

struct nvme_regs {
    volatile u64 cap;
    volatile u32 vs;
    volatile u32 intms;
    volatile u32 intmc;
    volatile u32 cc;
    volatile u32 rsvd1;
    volatile u32 csts;
    volatile u32 nssr;
    volatile u32 aqa;
    volatile u64 asq;
    volatile u64 acq;
} __attribute__((packed));

struct nvme_sqe {
    u32 cdw0;
    u32 nsid;
    u64 rsvd2;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} __attribute__((packed));

struct nvme_cqe {
    volatile u32 dw0;
    volatile u32 rsvd;
    volatile u32 sq_head_id;
    volatile u32 status;
} __attribute__((packed));

static void nvme_copy_trim(char *dst, const u8 *src, u32 length)
{
    u32 i;
    u32 end;

    for (i = 0U; i < length; i++) dst[i] = (char)src[i];

    end = length;
    while (end > 0U && (dst[end - 1U] == ' ' || dst[end - 1U] == '\0')) end--;

    dst[end] = '\0';
}

static volatile u32 *nvme_doorbell(u32 index, u32 stride)
{
    return (volatile u32 *)(NVME_MMIO_VADDR + NVME_REG_DOORBELL_BASE + (u64)index * stride);
}

static int nvme_wait_csts(struct nvme_regs *regs, u32 want_ready)
{
    u32 spins = 0U;

    while (((regs->csts & NVME_CSTS_RDY) != 0U) != (want_ready != 0U)) {
        spins++;
        if (spins > NVME_WAIT_SPINS) return -1;
    }

    return 0;
}

static int nvme_identify(u32 stride, struct nvme_sqe *sq, struct nvme_cqe *cq,
                          u32 *sq_tail, u32 *cq_head, u32 *phase,
                          u32 cns, u32 nsid, u32 cid, u32 data_phys)
{
    struct nvme_sqe *sqe = &sq[*sq_tail];
    struct nvme_cqe *cqe;
    u32 spins;

    sqe->cdw0  = NVME_OPC_IDENTIFY | (cid << 16);
    sqe->nsid  = nsid;
    sqe->rsvd2 = 0ULL;
    sqe->mptr  = 0ULL;
    sqe->prp1  = (u64)data_phys;
    sqe->prp2  = 0ULL;
    sqe->cdw10 = cns;
    sqe->cdw11 = 0U;
    sqe->cdw12 = 0U;
    sqe->cdw13 = 0U;
    sqe->cdw14 = 0U;
    sqe->cdw15 = 0U;

    *sq_tail = (*sq_tail + 1U) % NVME_ADMIN_QUEUE_ENTRIES;
    *nvme_doorbell(0U, stride) = *sq_tail;

    cqe = &cq[*cq_head];
    spins = 0U;
    while (((cqe->status >> 16U) & 1U) != *phase) {
        spins++;
        if (spins > NVME_WAIT_SPINS) return -1;
    }

    *cq_head = (*cq_head + 1U) % NVME_ADMIN_QUEUE_ENTRIES;
    if (*cq_head == 0U) *phase ^= 1U;
    *nvme_doorbell(1U, stride) = *cq_head;

    return (int)((cqe->status >> 17U) & 0x7FFFU);
}

void nvme_init(void)
{
    u8  bus;
    u8  device;
    u8  function;
    u64 bar_phys;
    struct nvme_regs *regs;
    u64 cap;
    u32 mqes;
    u32 dstrd;
    u32 stride;
    u32 queue_entries;
    u32 sq_phys;
    u32 cq_phys;
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    u32 sq_tail = 0U;
    u32 cq_head = 0U;
    u32 phase   = 1U;
    u32 cc;
    u32 id_phys;
    u8 *id_buf;
    int status;

    console_set_color(0x0BU);

    if (!pci_find_by_class(NVME_CLASS, NVME_SUBCLASS, &bus, &device, &function)) {
        console_set_color(0x0CU);
        console_printf("nvme: no NVMe controller found\n");
        return;
    }

    console_printf("nvme: found controller at %02X:%02X.%X\n", bus, device, function);

    pci_enable_device(bus, device, function);

    bar_phys = pci_bar_address(bus, device, function, 0U);

    page_map_mmio(NVME_MMIO_VADDR, (u32)bar_phys);
    page_map_mmio(NVME_MMIO_VADDR + 0x1000ULL, (u32)(bar_phys + 0x1000ULL));

    regs = (struct nvme_regs *)NVME_MMIO_VADDR;

    cap    = regs->cap;
    mqes   = (u32)(cap & 0xFFFFULL) + 1U;
    dstrd  = (u32)((cap >> 32U) & 0xFULL);
    stride = 4U << dstrd;

    console_printf("nvme: BAR0 phys=0x%016lX mqes=%u dstrd=%u stride=%u\n",
                   bar_phys, mqes, dstrd, stride);

    if (regs->cc & NVME_CC_EN) {
        regs->cc &= ~NVME_CC_EN;
        if (nvme_wait_csts(regs, 0U) != 0) {
            console_set_color(0x0CU);
            console_printf("nvme: controller did not disable in time\n");
            return;
        }
    }

    queue_entries = NVME_ADMIN_QUEUE_ENTRIES;
    if (queue_entries > mqes) queue_entries = mqes;

    sq_phys = page_alloc();
    cq_phys = page_alloc();

    if (sq_phys == 0U || cq_phys == 0U) {
        console_set_color(0x0CU);
        console_printf("nvme: admin queue allocation failed\n");
        return;
    }

    sq = (struct nvme_sqe *)((u64)sq_phys + KERNEL_OFFSET);
    cq = (struct nvme_cqe *)((u64)cq_phys + KERNEL_OFFSET);

    {
        u32 i;
        u8 *sq_bytes = (u8 *)sq;
        u8 *cq_bytes = (u8 *)cq;

        for (i = 0U; i < 0x1000U; i++) sq_bytes[i] = 0U;
        for (i = 0U; i < 0x1000U; i++) cq_bytes[i] = 0U;
    }

    regs->aqa = (queue_entries - 1U) | ((queue_entries - 1U) << 16U);
    regs->asq = (u64)sq_phys;
    regs->acq = (u64)cq_phys;

    cc = 0U;
    cc |= NVME_CC_EN;
    cc |= (6U << 16U);
    cc |= (4U << 20U);
    regs->cc = cc;

    if (nvme_wait_csts(regs, 1U) != 0) {
        console_set_color(0x0CU);
        console_printf("nvme: controller did not become ready in time\n");
        return;
    }

    console_set_color(0x0AU);
    console_printf("nvme: admin queue ready (%u entries)\n", queue_entries);

    id_phys = page_alloc();
    if (id_phys == 0U) {
        console_set_color(0x0CU);
        console_printf("nvme: identify buffer allocation failed\n");
        return;
    }
    id_buf = (u8 *)((u64)id_phys + KERNEL_OFFSET);

    status = nvme_identify(stride, sq, cq, &sq_tail, &cq_head, &phase,
                            NVME_CNS_CONTROLLER, 0U, 1U, id_phys);

    if (status != 0) {
        console_set_color(0x0CU);
        console_printf("nvme: identify controller failed status=0x%X\n", (u32)status);
    } else {
        char serial[21];
        char model[41];
        char firmware[9];

        nvme_copy_trim(serial, id_buf + 4, 20U);
        nvme_copy_trim(model, id_buf + 24, 40U);
        nvme_copy_trim(firmware, id_buf + 64, 8U);

        console_set_color(0x0AU);
        console_printf("nvme: identify controller model=\"%s\" serial=\"%s\" fw=\"%s\"\n",
                       model, serial, firmware);
    }

    {
        u32 i;
        u8 *id_bytes = id_buf;

        for (i = 0U; i < 0x1000U; i++) id_bytes[i] = 0U;
    }

    status = nvme_identify(stride, sq, cq, &sq_tail, &cq_head, &phase,
                            NVME_CNS_NAMESPACE, 1U, 2U, id_phys);

    if (status != 0) {
        console_set_color(0x0CU);
        console_printf("nvme: identify namespace failed status=0x%X\n", (u32)status);
        return;
    }

    {
        u64 nsze = *(volatile u64 *)id_buf;

        console_set_color(0x0AU);
        console_printf("nvme: identify namespace nsid=1 nsze=%lu blocks\n", nsze);
    }
}
