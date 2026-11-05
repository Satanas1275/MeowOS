#pragma once

/* FocalTech FT5452 on QUP0 SE2 (GENI I2C). 0 = no contact. */
void meow_touch_init(void);
int meow_touch_poll(int *x, int *y);
