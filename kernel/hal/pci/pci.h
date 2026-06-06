#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* PCI configuration space access and device enumeration. */

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* NVMe class/subclass */
#define PCI_CLASS_MASS_STORAGE    0x01
#define PCI_SUBCLASS_NVME         0x08

/* PCI type-0 configuration space header offsets */
#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_CLASS        0x0B
#define PCI_SUBCLASS     0x0A
#define PCI_PROG_IF      0x09
#define PCI_HEADER_TYPE  0x0E
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24

/* PCI command bits */
#define PCI_CMD_BUS_MASTER  (1 << 2)
#define PCI_CMD_MEM_SPACE   (1 << 1)

/* BAR flag */
#define PCI_BAR_IO          0x01
#define PCI_BAR_64BIT       0x04

typedef struct pci_dev {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor;
    uint16_t device;
    uint8_t  class;
    uint8_t  subclass;
    uint64_t bar[6];
    bool     bar_valid[6];
} pci_dev_t;

uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val);

bool     pci_scan(pci_dev_t *out, uint8_t class_id, uint8_t subclass_id);
uint64_t pci_read_bar(pci_dev_t *d, int bar_idx);
void     pci_enable_bus_master(pci_dev_t *d);
