# CubeMX — verify before you generate

The `.ioc` files in `firmware/projects/` **have been edited**. Your originals in
`Documents\` are untouched, so you can always fall back to them.

I can't run CubeMX, so I can't confirm it parses every key exactly as intended.
A key it doesn't recognise gets silently dropped — and a silently dropped
`AutoBusOff` looks exactly like everything working right up until a bus fault.
So: open each project, check the handful of things below, then Generate Code.

Every edit was applied by parsing the file, changing keys, and rewriting it
ASCII-sorted with CRLF endings — the same way CubeMX writes it. `git diff`
against your originals shows only the intended lines.

---

## The three active Bluepill projects

Identical changes to all three (1st/2nd/3rd). Only `Core/Inc/node_id.h`
differs between them, and that's a plain header CubeMX never touches.

A 4th Bluepill project still exists on disk but is retired: the system was
redesigned from 4 CAN nodes down to 3, and `firmware/bluepill_node/can_node.h`
now `#error`s if `NODE_ID >= TLM_NODE_COUNT` (3), which `NODE_ID=3` triggers.
Its checks below still apply if you ever wire it back in as a genuine 4th
node - the CubeMX fixes themselves aren't protocol-count-dependent - but its
`telemetry_proto.h`/`.c` copies were deliberately left unsynced, so treat it as
a stale reference, not a project you can flash today.

### Check 1 — CAN bit timing (the important one)

**Connectivity → CAN → Parameter Settings**

| Field | Should read |
|---|---|
| Prescaler | 9 |
| Time Quanta in Bit Segment 1 | 6 Times |
| Time Quanta in Bit Segment 2 | 1 Time |
| ReSynchronization Jump Width | 1 Time |

CubeMX computes and displays two derived values. **These are what actually
matter:**

- **Baud Rate: 500000**
- **Sample point: 87.5 %** (some versions show "Time for one Bit: 2000 ns")

If either is wrong, the timing keys didn't take — set them by hand in the GUI.

**Why.** Your original timing also gave exactly 500 kbit/s, so the bus worked.
But it sampled each bit at 77.8 % while the Nucleo sampled at 87.5 %. That
9.7-point mismatch eats the margin the bus needs for oscillator tolerance and
propagation delay: fine on a short bench harness, sporadic form and stuff
errors once cables get longer or a board warms up. 87.5 % is CiA's recommended
point, and the Nucleo already sits there.

At PCLK1 = 36 MHz there's no combination with BS2 = 2 that hits 87.5 % exactly,
which is why BS2 drops to 1 tq. bxCAN allows it. If you'd rather keep more
margin on the phase-2 segment, **Prescaler 4 / BS1 14 / BS2 3** gives 83.3 % —
still far better than 77.8 %.

### Check 2 — Automatic Bus-Off Management

Same panel, scroll down:

| Field | Should read |
|---|---|
| Automatic Bus-Off Management | **Enabled** |
| Automatic Retransmission | Disabled |

Was disabled. With it off, one shorted wire or missing terminator drives the
controller to bus-off and the node stays mute until power-cycled, with nothing
to indicate why.

Automatic Retransmission stays **disabled** on purpose — one-shot is right for
periodic telemetry. A frame that loses arbitration is better replaced by the
next sample 100 ms later than retried with data that's already stale.

### Check 3 — SWD is back

**System Core → SYS**

| Field | Should read |
|---|---|
| Debug | **Serial Wire** |

In the pinout view, **PA13** and **PA14** should be green and labelled
`SYS_JTMS-SWDIO` / `SYS_JTCK-SWCLK`.

Was `No Debug`, which frees PA13/PA14 as GPIO. Once that firmware runs, SWD is
dead and the only way back in is jumpering BOOT0 high and flashing over serial.

---

## Nucleo CAN Bus Test (the hub)

### Check 1 — CAN1

**Connectivity → CAN1 → Parameter Settings**

| Field | Should read |
|---|---|
| Prescaler / BS1 / BS2 | 10 / 13 / 2 — **unchanged** |
| Baud Rate | 500000 |
| Sample point | 87.5 % |
| Automatic Bus-Off Management | **Enabled** ← the only change |

### Check 2 — USART1

**Connectivity → USART1**

| Field | Should read |
|---|---|
| Mode | Asynchronous |
| Baud Rate | **9600** |
| Word Length / Parity / Stop Bits | 8 Bits / None / 1 |
| Hardware Flow Control | Disable |

Pinout should show **PA9 = USART1_TX**, **PA10 = USART1_RX**.

9600 is the E22's factory serial rate. This is the wire rate between the STM32
and the module, **not** the over-the-air rate — those are configured separately,
and confusing them is the usual reason a LoRa link stays silent.

### Check 3 — the DMA request

**Connectivity → USART1 → DMA Settings** — there should be one entry:

| Field | Should read |
|---|---|
| DMA Request | USART1_TX |
| Stream/Channel | **DMA1 Channel 4** |
| Direction | Memory To Peripheral |
| Priority | Low |
| Mode | Normal |
| Increment | Memory ✓, Peripheral ✗ |
| Data Width | Byte / Byte |

This is the entry most likely to have been dropped, because DMA keys are the
most version-sensitive part of the `.ioc` format. **If the DMA Settings tab is
empty, add it by hand** — click Add, pick `USART1_TX`, and set the fields above.

Without it, `lora_e22_send()` calls `HAL_UART_Transmit_DMA` on an unlinked
handle and returns an error every time. The radio never transmits, and nothing
in the debug output says why.

### Check 4 — interrupts

**System Core → NVIC**

| Interrupt | Enabled | Preemption priority |
|---|---|---|
| CAN1 RX0 interrupt | ✓ | **0** |
| USART1 global interrupt | ✓ | **1** |
| DMA1 channel4 global interrupt | ✓ | **1** |

CAN must preempt the other two — a radio transfer completing should never delay
servicing the CAN receive FIFO.

**CAN1 RX0 was already enabled in your original**, and `stm32l4xx_it.c` already
had the handler. Worth confirming it survived: if that interrupt is off, the
hub reports all three nodes permanently stale and nothing points at the cause.

USART1's interrupt is what lets the DMA transfer-complete callback fire.
Without it exactly one frame goes out and then the radio goes quiet forever.

### Check 5 — GPIO

**System Core → GPIO**, four pins:

| Pin | Mode | Pull | Output level | Label |
|---|---|---|---|---|
| PB0 | Output Push Pull | No pull-up/down | Low | `E22_M0` |
| PA4 | Output Push Pull | No pull-up/down | Low | `E22_M1` |
| PA1 | Input | **Pull-up** | — | `E22_AUX` |
| PB6 | Output Push Pull | **Pull-up** | **High** | `SD_CS` |

M0 = M1 = 0 is the E22's transparent transmission mode; driving them low from
reset avoids a spurious mode change during startup. AUX is an open-drain output
on the module, so it needs a pull-up on our side. SD chip select must idle high
or the card misreads its first command.

M1 and AUX sit on PA4/PA1 rather than PB1/PB2 so all five E22 signal wires
(M0, M1, AUX, RXD, TXD) land on the Nucleo's Arduino Uno R3 header (A2, A1,
A3, D8, D2) instead of needing the Morpho connector for two of them.

### Check 6 — SPI1 should NOT be there

PA5, PA6 and PA7 should be **unassigned**, and there should be no SPI1 entry.

That's deliberate. Your project has no `stm32l4xx_hal_spi.c`, so `sd_spi.c`
drives the card through the SPI1 registers directly — it configures the pins,
enables the clock and sets the baud rate itself.

Adding SPI1 in CubeMX would pull in the HAL SPI driver and generate an
`MX_SPI1_Init()` that `sd_spi_init()` immediately overrides. Harmless, but it
makes it look like two things own the peripheral.

---

## Then: Generate Code

`ProjectManager.functionlistsort` still lists the old function order — CubeMX
rewrites that itself on generate, so ignore it.

### What regeneration overwrites

It rewrites `main.c`, `*_hal_msp.c` and `*_it.c` **except** for anything between
`/* USER CODE BEGIN X */` and `/* USER CODE END X */`.

Those files currently contain hand-written equivalents of what CubeMX will
produce — deliberately, so the projects build and run even if you never open
CubeMX. Generating replaces them with CubeMX's own versions, which is the
outcome you want.

The calls the firmware needs live in USER CODE blocks and survive:

| Project | Block | Content |
|---|---|---|
| Bluepill | Includes | `#include "can_node.h"` |
| Bluepill | 2 | `can_node_init(&hcan);` |
| Bluepill | 3 | `can_node_task();` |
| Nucleo | Includes | `#include "telemetry_hub.h"` |
| Nucleo | 2 | `hub_init(&hcan1, &huart1, &huart2);` |
| Nucleo | 3 | `hub_task();` |

If any go missing after generating, put them back — CubeMX occasionally drops
USER CODE when a peripheral is deleted.

### Spot-check the generated code

**Bluepill** `Core/Src/main.c`:

```c
hcan.Init.Prescaler = 9;
hcan.Init.TimeSeg1 = CAN_BS1_6TQ;
hcan.Init.TimeSeg2 = CAN_BS2_1TQ;
hcan.Init.AutoBusOff = ENABLE;
```

**Bluepill** `Core/Src/stm32f1xx_hal_msp.c` — `__HAL_AFIO_REMAP_SWJ_NOJTAG();`
and **not** `__HAL_AFIO_REMAP_SWJ_DISABLE();`

**Nucleo** `Core/Src/main.c` — `hcan1.Init.AutoBusOff = ENABLE;`,
`huart1.Init.BaudRate = 9600;`, and `MX_DMA_Init()` + `MX_USART1_UART_Init()`
called from `main()`.

**Nucleo** `Core/Src/stm32l4xx_hal_msp.c` — a `HAL_UART_MspInit` branch for
USART1 that sets up `hdma_usart1_tx` and calls `__HAL_LINKDMA`.

**Nucleo** `Core/Src/stm32l4xx_it.c` — `CAN1_RX0_IRQHandler`,
`USART1_IRQHandler`, `DMA1_Channel4_IRQHandler`.

### Finally

Delete any leftover `CAN_Sender_Init` / `CAN_Sender_Loop` / `CAN_Receiver_Init`
/ `HAL_CAN_RxFifo0MsgPendingCallback` from the old `USER CODE BEGIN 0` blocks.
The duplicate callback fails at link time, which is the good outcome — it means
you can't accidentally ship both.

---

## Adding pins later

Nothing here locks the `.ioc`. Open it in CubeMX, click a pin, assign it,
regenerate — exactly as if you'd configured it yourself from the start. The
files are ordinary CubeMX format; the only reason to check them this once is
that I wrote them without being able to open the tool.
