#include "kernel/hal/pci/pci.h"
#include "kernel/arch/x86_64/io.h"
#include "kernel/lib/kprintf.h"

uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)(0x80000000
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)dev  << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC));
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

void pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (uint32_t)(0x80000000
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)dev  << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC));
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

bool pci_scan(pci_dev_t *out, uint8_t class_id, uint8_t subclass_id) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t hdr0 = pci_read((uint8_t)bus, dev, 0, 0);
            uint16_t vendor = hdr0 & 0xFFFF;
            if (vendor == 0xFFFF) continue;

            uint32_t class_reg = pci_read((uint8_t)bus, dev, 0, 8);
            uint8_t cls = (class_reg >> 24) & 0xFF;
            uint8_t sub = (class_reg >> 16) & 0xFF;

            if (cls == class_id && sub == subclass_id) {
                out->bus  = (uint8_t)bus;
                out->dev  = dev;
                out->func = 0;
                out->vendor = vendor;
                out->device = (hdr0 >> 16) & 0xFFFF;
                out->class  = cls;
                out->subclass = sub;
                for (int i = 0; i < 6; i++) {
                    out->bar[i] = pci_read_bar(out, i);
                    out->bar_valid[i] = (out->bar[i] != 0);
                }
                return true;
            }

            /* Check multifunction bit */
            uint8_t hdr_type = (pci_read((uint8_t)bus, dev, 0, 14) >> 16) & 0x7F;
            if (hdr_type & 0x80) {
                for (uint8_t func = 1; func < 8; func++) {
                    uint32_t fhdr = pci_read((uint8_t)bus, dev, func, 0);
                    if ((fhdr & 0xFFFF) == 0xFFFF) continue;
                    uint32_t fclass = pci_read((uint8_t)bus, dev, func, 8);
                    uint8_t fcls = (fclass >> 24) & 0xFF;
                    uint8_t fsub = (fclass >> 16) & 0xFF;
                    if (fcls == class_id && fsub == subclass_id) {
                        out->bus  = (uint8_t)bus;
                        out->dev  = dev;
                        out->func = func;
                        out->vendor = fhdr & 0xFFFF;
                        out->device = (fhdr >> 16) & 0xFFFF;
                        out->class  = fcls;
                        out->subclass = fsub;
                        for (int i = 0; i < 6; i++) {
                            out->bar[i] = pci_read_bar(out, i);
                            out->bar_valid[i] = (out->bar[i] != 0);
                        }
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

uint64_t pci_read_bar(pci_dev_t *d, int bar_idx) {
    uint8_t offset = PCI_BAR0 + bar_idx * 4;
    uint32_t bar_lo = pci_read(d->bus, d->dev, d->func, offset);
    if (bar_lo == 0) return 0;

    if (bar_lo & PCI_BAR_IO) {
        return bar_lo & 0xFFFFFFFC;
    }

    /* Memory BAR */
    if (bar_lo & PCI_BAR_64BIT) {
        uint32_t bar_hi = pci_read(d->bus, d->dev, d->func, offset + 4);
        return ((uint64_t)bar_hi << 32) | (bar_lo & 0xFFFFFFF0);
    }
    return bar_lo & 0xFFFFFFF0;
}

void pci_enable_bus_master(pci_dev_t *d) {
    uint32_t cmd = pci_read(d->bus, d->dev, d->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
    pci_write(d->bus, d->dev, d->func, PCI_COMMAND, cmd);
}
