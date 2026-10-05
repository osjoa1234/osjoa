#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "console.h"

void keyboard_init(void);
void keyboard_irq(void);
char keyboard_getchar(void);

#endif
