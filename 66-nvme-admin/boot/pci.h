#ifndef PCI_H
#define PCI_H

#include "console.h"

void pci_scan(void);
int  pci_find_by_class(u8 class_code, u8 subclass, u8 *out_bus, u8 *out_device, u8 *out_function);
void pci_enable_device(u8 bus, u8 device, u8 function);
u64  pci_bar_address(u8 bus, u8 device, u8 function, u32 bar_index);

#endif
