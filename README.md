# boost_bench — RP2350 / Pico 2 boost-switching overhead

Measures what it actually costs to jump from 150 MHz / 1.10 V to 500 MHz / 1.60 V
and back, on your board.

**This drives the core rail about 45% above its rated maximum. Use a board you
can afford to lose.**

## Which file to flash

| File | Use it when |
|---|---|
| `boost_bench_ram.uf2` | **Start here.** Whole image runs from SRAM, so flash timing cannot crash it. |
| `boost_bench_flash.uf2` | Once the RAM build works. Executes over XIP, so it also exercises QMI retiming — closer to a real application. |

Hold BOOTSEL, plug in, drag the `.uf2` onto `RPI-RP2`. Output appears on USB
serial (`/dev/ttyACM0`, or `COM*`), and in parallel on UART0 at 115200 on GP0/GP1.
It waits up to 10 s for a USB terminal before starting.

GP15 goes high for the duration of every boost window — probe it alongside the
core rail if you have a scope.

## What each test tells you

1. **Flash (QMI) timing** — sweeps `CLKDIV` and `RXDELAY` at 500 MHz and CRCs a
   flash window through the uncached XIP alias to find which combinations read
   correctly. Prints a `.`/`x` map. This runs entirely from SRAM with interrupts
   off, so a wrong `RXDELAY` corrupts data reads but can never corrupt
   instruction fetch — which is what makes a blind search safe.
2. **Component costs** — the POWMAN register write, the QMI retime, and the
   clock change itself, with the rail already high so no settling is mixed in.
   Compares `set_sys_clock_khz()` against a postdiv-only path (see below).
3. **VOUT_OK** — whether `POWMAN_VREG_STS.VOUT_OK` reacts to the voltage step at
   all, and if so how long it takes to come back. If it never deasserts, it is
   useless as a settle-complete signal and test 4 is the only real answer.
4. **Hardware SHA-256** — hashes a fixed 4 KiB message 64 times in each of
   three phases: at 150 MHz/1.10 V, at 500 MHz/1.60 V, and again at
   150 MHz/1.10 V after the boost. Reports per-hash time, MB/s and cycles per
   byte for each phase, verifies the digest on every single hash, and counts
   `ERR_WDATA_NOT_RDY` feed underruns. At startup the digest is also checked
   against a known answer computed off-device, so a pass proves the block is
   correct rather than merely self-consistent. The "after" phase exists to
   catch a block that got into a bad state during the overclock and stayed
   there — a failure there is much worse news than a failure during.
5. **Settle-delay sweep** — the one that matters. Walks the delay between the
   voltage write and the clock rise downward, verifying a deterministic
   multiply/memory workload, a flash CRC and a SHA-256 digest after each
   boost — three independent detectors (core ALU, flash interface, SHA
   block), so a marginal rail shows up whichever path fails first. Stops at the first
   delay that computes a wrong answer. A watchdog catches hangs and the bad
   delay is remembered in scratch across the reboot, so a crash does not cost
   you the whole run.
6. **Round trip** — min/mean/max of enter+exit at 3× the shortest surviving
   settle delay.
7. **Throughput and break-even** — real speedup on the same workload, and the
   workload duration above which boosting is worth it.

Menu after the run: `r` rerun, `s` settle sweep only, `c` forget remembered
crash points.

## The postdiv shortcut

On RP2350 both frequencies come off the same 1500 MHz VCO:

```
150 MHz = 1500 / (5 × 2)
500 MHz = 1500 / (3 × 1)
```

So the clock can be changed by parking `clk_sys` on `clk_ref`, rewriting
`PLL_SYS->PRIM`, and parking back — the VCO never unlocks and the PLL block is
never reset. `set_sys_clock_khz()` does a full `pll_init()` including a block
reset and a lock wait, and parks on `pll_usb` at 48 MHz while it does so. The
benchmark measures both so you can see the difference.

If you change `BOOST_KHZ`, you **must** update `BOOST_PD1`/`BOOST_PD2` to a pair
that divides 1500 MHz to your new target, or drop the fast path. Otherwise
`fast_set_prim()` will set a frequency that does not match what the code claims.

## Reading the numbers

The SHA-256 block is clocked from `clk_sys`, so its throughput should scale
close to the 3.33× clock ratio. Cycles-per-byte is the number to compare across
phases — it should stay flat. If it rises while boosted, the bottleneck is the
CPU's ready-polling or flash fetch, not the accelerator.

Expect the voltage settle delay to dominate everything else by a wide margin.
The clock change and the register writes are microseconds; settling is
hundreds of microseconds. If the sweep reports that even 0 µs passes, that means
the rail was already high enough before the clock rose — it is margin, not proof
that no settling is needed, and it will shrink as the die warms up. Re-run after
the board has been hot for a while.

## Rebuilding

```
export PICO_SDK_PATH=/path/to/pico-sdk      # 2.x, with lib/tinyusb checked out
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

Built and verified here against pico-sdk 2.2.0, `PICO_BOARD=pico2`,
`PICO_PLATFORM=rp2350-arm-s`, arm-none-eabi-gcc 13.2.

Knobs at the top of `boost_bench.c`: `BASE_KHZ`, `BOOST_KHZ`, `BASE_VREG_LEVEL`,
`BOOST_VREG_LEVEL`, `MARKER_PIN`, `RAIL_DECAY_MS`. Lower `BOOST_VREG_LEVEL` to
`VREG_VOLTAGE_1_30` first if you want to see the whole thing run without going
out of spec.
