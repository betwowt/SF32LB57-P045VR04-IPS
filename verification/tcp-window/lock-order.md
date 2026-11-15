# SDIO lock-order evidence

Read-only console capture: `stall-state.log`, firmware `build-stress-07.log`.
At this point allocation failures were already zero, but WebSocket had stalled.

| Thread | Held mutex | Waiting for |
| --- | --- | --- |
| `fhost_tx` | `sdiodev-` (`sdio_dev.bus_txrx`) | `sd_bus_l` (MMC host) |
| `sdio_irq` | `sd_bus_l` (MMC host) | `sdiodev-` (`sdio_dev.bus_txrx`) |

The console showed both mutexes held once with one waiter. TX had six queued
notifications, seven outstanding network tags, and five free SDIO RX nodes.
This was not RX node exhaustion or lack of heap space.

The dependency is visible in source:

1. RT-Thread `components/drivers/sdio/sdio.c:sdio_irq_thread()` locks the MMC
   host, reads the pending function bits, then calls `func->irq_handler()`.
2. AIC `platform/hal/sdio_port.c:aicwf_sdio_irq_hdl()` runs synchronous RX when
   `config_rxtask_insdio` is set.
3. AIC CMD52/CMD53 wrappers acquire `sdio_dev.bus_txrx`.
4. RT-Thread `mmcsd_core.c:mmcsd_send_request()` acquires the MMC host lock for
   every request, including TX requests already holding `bus_txrx`.

The fix releases the dispatcher's outer MMC host lock around the AIC callback,
then reacquires it before returning. Every actual command still goes through
`mmcsd_send_request()` and its host lock. The AIC wrappers keep `bus_txrx` around
their transfers. TX and synchronous RX therefore share the same order:
`bus_txrx -> MMC host`.

The RT-Thread dispatcher and other SDIO drivers are unchanged. No driver
priority changes are retained.
