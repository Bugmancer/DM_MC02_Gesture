# Gesture Display

This module adapts the LCD initialization sequence and the ASCII 8x16 font from
`DM_MC02_BusScope/User`. The screen is 280x240 in landscape orientation, with
MADCTL `0x70` and a 20-pixel controller X offset.

## Integration

Add `Display/display.c` to the target and `Display` to the include path. Call
`display_init()` after `MX_GPIO_Init()` and `MX_SPI1_Init()`, before starting the
sample timer. Initialization takes approximately 320 ms because of the panel's
reset and sleep-out delays. A return value of zero reports a transport or DMA
memory-placement problem; it does not test physical panel presence.

Call `display_set_view()` and `display_process()` from the main loop only. The
view is copied, so the caller may reuse its structure. Slot indices are zero
based; use `best_slot = -1` and negative scores when no result is available.
Scores outside `[0, 1000)` are displayed as `---`. Messages are copied and
terminated, with 39 visible ASCII characters at most; lowercase is rendered as
uppercase. `power_ok` means the application's power-enable state, not a measured
supply voltage. Use the message for acquisition or storage error details.

## DMA And Timing

- Place the `.dma_buffer` section in STM32H723 AXI SRAM, `0x24000000` through
  `0x2404FFFF`. Initialization rejects DTCM placement. The DMA buffer is aligned
  to 32 bytes, and its 2240-byte extent covers whole cache lines.
- Keep generated SPI1 and DMA1 Stream 2 IRQ handlers enabled. The module polls
  `HAL_SPI_STATE_READY`; the H7 HAL sets this after SPI EOT, so both CS and DC
  remain unchanged until the final pixel has left the peripheral. No application
  SPI callbacks are consumed.
- Each `display_process()` submits at most one 280x4 RGB565 transfer and never
  waits for DMA completion. At the configured SPI1 clock of 30 MHz, the wire time
  is approximately 0.598 ms per tile, plus short address commands. CPU and actual
  bus timing still require measurement on the board.
- Text rows are compared and only changed rows are sent. View composition is
  limited to 10 Hz. Each active row has a stable snapshot across its four tiles,
  and its DMA buffer is not touched while busy. Refresh does not clear the screen.
- Runtime state uses approximately 3.5 KiB RAM, including the DMA buffer. The font
  occupies 1 KiB flash. There is no full-screen framebuffer or heap allocation.
- If a transfer fails or remains busy for over 20 ms, `display_is_ok()` becomes
  zero and refresh stops. On a busy timeout the module leaves CS/DC/buffer alone;
  acquisition can continue, and a later application reset can reinitialize LCD.

## Host Checks

From the project directory, using GCC:

```powershell
gcc -std=c99 -Wall -Wextra -Werror -I Display/tests Display/tests/test_display.c -o $env:TEMP\dm_gesture_display_test.exe
& $env:TEMP\dm_gesture_display_test.exe
```

The five transport tests cover first-frame/backlight completion, unchanged-view
suppression, view changes during DMA, timeout/start-failure behavior, and message
termination. Target initialization, pixel appearance and real DMA interrupt
timing require board validation.
