/* X OS compat: bus_dma - define types needed by x86/bus_dma.h and busdma_impl.h */
#ifndef _XOS_COMPAT_BUS_DMA_H
#define _XOS_COMPAT_BUS_DMA_H

#include <sys/cdefs.h>
#include <sys/_types.h>

/* Define bus DMA types - these are normally in machine/bus_dma.h */
#ifndef _BUS_ADDR_T_DEFINED
typedef uint64_t bus_addr_t;
#define _BUS_ADDR_T_DEFINED
#endif
#ifndef _BUS_SIZE_T_DEFINED
typedef size_t bus_size_t;
#define _BUS_SIZE_T_DEFINED
#endif

struct bus_dma_tag_common;
typedef struct bus_dma_tag_common *bus_dma_tag_t;
typedef void *bus_dmamap_t;
typedef struct bus_dma_segment {
    bus_addr_t ds_addr;
    bus_size_t ds_len;
} bus_dma_segment_t;

typedef void bus_dma_lock_t(void *lock_arg, int op);
typedef void bus_dmamap_callback_t(void *callback_arg, bus_dma_segment_t *segs,
    int nseg, int error);
typedef int bus_dmasync_op_t;

#define BUS_DMA_WAITOK   0x00
#define BUS_DMA_NOWAIT   0x01
#define BUS_DMA_ZERO     0x02
#define BUS_DMA_COHERENT 0x04

/* bus_dma functions are provided by x86/bus_dma.h as static inline */

#endif /* _XOS_COMPAT_BUS_DMA_H */
