#pragma once

/* MEOW OS: terminal + on-screen keyboard. Never returns. */
void meow_run(void);
void meow_ui_render(void);
void meow_ui_demo(void);	/* host_test: draw the UI without touching hardware */
