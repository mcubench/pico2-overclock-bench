# pico2-overclock-bench - RP2350 / Pico 2 boost-switching overhead

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

## Summary results on Feitian PICO2_G9_QE09_v1.0 RP2350 token

<img width="1830" height="516" alt="image" src="https://github.com/user-attachments/assets/07ad1456-96aa-4cf8-8336-2a78448aef3c" />

Summary: it costs only `101 us` (postdiv path) to switch to 500MHz@1.6V (mainly waiting for 1.6V to stabilize). Switch back is only `~50us` as waiting for 1.1V is not necessary.

| Operation | `boost_bench_flash.uf2` | `boost_bench_ram.uf2` |
|---|---|---|
| voltage register write          | 21.332 us | 21.334 us |                  
| QMI retime + dummy read         | 0.880 us | 0.840 us |
| 150->500 via set_sys_clock_khz  | 94.780 us | 93.340 us |  
| 500->150 via set_sys_clock_khz  | 66.510 us | 66.305 us |
| 150->500 via postdiv only       | 4.005 us | 4.000 us |
| 500->150 via postdiv only       | 3.001 us | 3.000 us |
| SHA-256 before / during / after | 36.30 / 120.97 / 36.30 MB/s | 36.31 / 121.02 / 36.31 MB/s |
| SHA-256 digest errors           | 0 / 0 / 0 | 0 / 0 / 0 |
| settle delay used               | 50 us | 50 us |
| round trip, SDK path            | 259 us | 255 us |
| round trip, postdiv path        | 101 us | 101 us |



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

## Full results on Feitian PICO2_G9_QE09_v1.0 RP2350 token

### Run from Flash memory: `boost_bench_flash.uf2`

```bash
=========================================================
 boost_bench - RP2350 dynamic clock/voltage overhead
 flash XIP build | 150 -> 500 MHz | VSEL 0x0b -> 0x13
 WARNING: drives the core rail far above its rated maximum.
 GP15 is high while boosted - probe it against the core rail.
=========================================================
  clk_sys 150000 kHz  clk_peri 150000 kHz  clk_ref 12000 kHz
  VREG VSEL=0x0b  VREG_CTRL=0x00008150  VREG_STS=0x00000011
  PLL_SYS fbdiv=125 prim=0x00052000 cs=0x80000001 -> VCO 1500 MHz
  QMI M0_TIMING=0x60007203

  golden hash=0x89da1ff4  crc1k=0x7bb2b355  crc4k=0x6b6fb1b8
  SHA-256 hw digest  = a5ce8d318fdb51f7ca57aaff5b77d83e5b804cb9ed3e0efe045006e440e8cdc5
  known-answer test  : PASS

------------------- run start -------------------

== 1. Flash (QMI) timing at 500 MHz ==
  boot value M0_TIMING=0x60007203 -> CLKDIV=3 RXDELAY=2 (QSPI 50 MHz at base)
  CLKDIV=10  (QSPI 50 MHz)  RXDELAY ........   
  CLKDIV=11  (QSPI 45 MHz)  RXDELAY ........   
  CLKDIV=12  (QSPI 41 MHz)  RXDELAY ........   
  ('.' = correct CRC, 'x' = wrong, RXDELAY 0 on the left)
  chosen: CLKDIV=10 RXDELAY=3 (window 0..7, centred)

== 2. Component costs (rail held high - no settling in these) ==
  vreg_set_voltage() register handshake : 21.332 us
  set_qmi() write + dummy XIP read      : 0.880 us
  SDK set_sys_clock_khz  150->500             : 94.780 us
                         500->150             : 66.510 us
  postdiv-only (RAM)     150->500             : 4.005 us
                         500->150             : 3.001 us

== 3. POWMAN VREG_STS.VOUT_OK on a 1.10 -> 1.60 V step ==
  VOUT_OK never deasserted - the comparator does not treat this
  step as out-of-regulation, so it is useless as a settle signal.
  Test 4 is the one that tells you the real number.

== 4. Hardware SHA-256 accelerator ==
   4096-byte message, 64 hashes per phase, digest checked every time.
  before boost           150 MHz  112.828 us/hash  36.30 MB/s  4.1 cyc/B  all digests ok
  during boost           500 MHz  33.859 us/hash  120.97 MB/s  4.1 cyc/B  all digests ok
  after boost            150 MHz  112.812 us/hash  36.30 MB/s  4.1 cyc/B  all digests ok
   digest: a5ce8d318fdb51f7ca57aaff5b77d83e5b804cb9ed3e0efe045006e440e8cdc5
   known answer MATCHES
   boosted speedup 3.33x on the SHA block

== 5. Settle-delay sweep via postdiv-only (RAM) ==
   Long delays first; stops at the first delay that computes wrong.
   Each trial checks the core workload, a flash CRC and a SHA-256 digest.
   settle    enter     exit      verdict
    4000 us    4025 us     26 us  ok
    2000 us    2025 us     25 us  ok
    1000 us    1025 us     28 us  ok
     600 us     626 us     28 us  ok
     400 us     425 us     25 us  ok
     250 us     276 us     26 us  ok
     150 us     176 us     25 us  ok
     100 us     126 us     27 us  ok
      60 us      85 us     24 us  ok
      30 us      54 us     27 us  ok
      10 us      36 us     26 us  ok
       0 us      25 us     25 us  ok
   shortest delay still computing correctly: 0 us
   using 50 us (3x margin) for test 6
   NOTE: 0 us passed, meaning the rail was already high enough
   before the clock rose. That is margin, not proof that no
   settling is needed - it will shrink as the die heats up.

== 6. Full round trip, settle = 50 us ==
  SDK set_sys_clock_khz  enter+exit  min 255 / mean 259 / max 601 us
  postdiv-only (RAM)     enter+exit  min 101 / mean 101 / max 111 us

== 7. Throughput and break-even ==
  identical workload: 51890 us at 150 MHz, 15568 us at 500 MHz
  measured speedup 3.33x  (clock ratio 3.33x)
  SDK set_sys_clock_khz  pays for itself above 370 us of 150 MHz work
  postdiv-only (RAM)     pays for itself above 144 us of 150 MHz work

== Summary ==
  voltage register write          : 21.332 us
  QMI retime + dummy read         : 0.880 us
  150->500 via set_sys_clock_khz  : 94.780 us
  500->150 via set_sys_clock_khz  : 66.510 us
  150->500 via postdiv only       : 4.005 us
  500->150 via postdiv only       : 3.001 us
  SHA-256 before / during / after : 36.30 / 120.97 / 36.30 MB/s
  SHA-256 digest errors           : 0 / 0 / 0
  settle delay used               : 50 us
  round trip, SDK path            : 259 us
  round trip, postdiv path        : 101 us

  Everything except the settle delay is rounding error. Tune the
  settle delay on YOUR board with the die warm, then keep margin.
-------------------- run end --------------------
```

### Run from RAM memory: `boost_bench_ram.uf2`

```bash
=========================================================
 boost_bench - RP2350 dynamic clock/voltage overhead
 copy_to_ram build | 150 -> 500 MHz | VSEL 0x0b -> 0x13
 WARNING: drives the core rail far above its rated maximum.
 GP15 is high while boosted - probe it against the core rail.
=========================================================
  clk_sys 150000 kHz  clk_peri 150000 kHz  clk_ref 12000 kHz
  VREG VSEL=0x0b  VREG_CTRL=0x00008150  VREG_STS=0x00000011
  PLL_SYS fbdiv=125 prim=0x00052000 cs=0x80000001 -> VCO 1500 MHz
  QMI M0_TIMING=0x60007203

  golden hash=0x89da1ff4  crc1k=0xd2c2442e  crc4k=0x3f9cd131
  SHA-256 hw digest  = a5ce8d318fdb51f7ca57aaff5b77d83e5b804cb9ed3e0efe045006e440e8cdc5
  known-answer test  : PASS

------------------- run start -------------------

== 1. Flash (QMI) timing at 500 MHz ==
  boot value M0_TIMING=0x60007203 -> CLKDIV=3 RXDELAY=2 (QSPI 50 MHz at base)
  CLKDIV=10  (QSPI 50 MHz)  RXDELAY ........   
  CLKDIV=11  (QSPI 45 MHz)  RXDELAY ........   
  CLKDIV=12  (QSPI 41 MHz)  RXDELAY ........   
  ('.' = correct CRC, 'x' = wrong, RXDELAY 0 on the left)
  chosen: CLKDIV=10 RXDELAY=3 (window 0..7, centred)

== 2. Component costs (rail held high - no settling in these) ==
  vreg_set_voltage() register handshake : 21.334 us
  set_qmi() write + dummy XIP read      : 0.840 us
  SDK set_sys_clock_khz  150->500             : 93.340 us
                         500->150             : 66.305 us
  postdiv-only (RAM)     150->500             : 4.000 us
                         500->150             : 3.000 us

== 3. POWMAN VREG_STS.VOUT_OK on a 1.10 -> 1.60 V step ==
  VOUT_OK never deasserted - the comparator does not treat this
  step as out-of-regulation, so it is useless as a settle signal.
  Test 4 is the one that tells you the real number.

== 4. Hardware SHA-256 accelerator ==
   4096-byte message, 64 hashes per phase, digest checked every time.
  before boost           150 MHz  112.781 us/hash  36.31 MB/s  4.1 cyc/B  all digests ok
  during boost           500 MHz  33.843 us/hash  121.02 MB/s  4.1 cyc/B  all digests ok
  after boost            150 MHz  112.796 us/hash  36.31 MB/s  4.1 cyc/B  all digests ok
   digest: a5ce8d318fdb51f7ca57aaff5b77d83e5b804cb9ed3e0efe045006e440e8cdc5
   known answer MATCHES
   boosted speedup 3.33x on the SHA block

== 5. Settle-delay sweep via postdiv-only (RAM) ==
   Long delays first; stops at the first delay that computes wrong.
   Each trial checks the core workload, a flash CRC and a SHA-256 digest.
   settle    enter     exit      verdict
    4000 us    4024 us     25 us  ok
    2000 us    2024 us     25 us  ok
    1000 us    1024 us     29 us  ok
     600 us     624 us     29 us  ok
     400 us     424 us     25 us  ok
     250 us     274 us     26 us  ok
     150 us     174 us     25 us  ok
     100 us     124 us     27 us  ok
      60 us      84 us     24 us  ok
      30 us      54 us     27 us  ok
      10 us      34 us     27 us  ok
       0 us      24 us     25 us  ok
   shortest delay still computing correctly: 0 us
   using 50 us (3x margin) for test 6
   NOTE: 0 us passed, meaning the rail was already high enough
   before the clock rose. That is margin, not proof that no
   settling is needed - it will shrink as the die heats up.

== 6. Full round trip, settle = 50 us ==
  SDK set_sys_clock_khz  enter+exit  min 255 / mean 255 / max 257 us
  postdiv-only (RAM)     enter+exit  min 101 / mean 101 / max 102 us

== 7. Throughput and break-even ==
  identical workload: 51890 us at 150 MHz, 15567 us at 500 MHz
  measured speedup 3.33x  (clock ratio 3.33x)
  SDK set_sys_clock_khz  pays for itself above 364 us of 150 MHz work
  postdiv-only (RAM)     pays for itself above 144 us of 150 MHz work

== Summary ==
  voltage register write          : 21.334 us
  QMI retime + dummy read         : 0.840 us
  150->500 via set_sys_clock_khz  : 93.340 us
  500->150 via set_sys_clock_khz  : 66.305 us
  150->500 via postdiv only       : 4.000 us
  500->150 via postdiv only       : 3.000 us
  SHA-256 before / during / after : 36.31 / 121.02 / 36.31 MB/s
  SHA-256 digest errors           : 0 / 0 / 0
  settle delay used               : 50 us
  round trip, SDK path            : 255 us
  round trip, postdiv path        : 101 us

  Everything except the settle delay is rounding error. Tune the
  settle delay on YOUR board with the die warm, then keep margin.
-------------------- run end --------------------
```
