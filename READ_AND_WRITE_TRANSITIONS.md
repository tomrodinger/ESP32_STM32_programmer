# Read/write transitions in SWD (between transactions)

This note focuses on what should happen on the wire **between** a read and a subsequent write (and vice versa), and what we will implement in this repo so the simulations are robust.

Scope:

* The two standalone sims [`read_then_write_simulation_main.cpp`](sim/read_then_write_simulation_main.cpp) and [`write_then_read_simulation_main.cpp`](sim/write_then_read_simulation_main.cpp).
* The SWD host bit-bang implementation in [`swd_min.cpp`](src/swd_min.cpp).
* The simulated target state machine in [`Stm32SwdTarget::on_swclk_rising_edge()`](sim/stm32_swd_target.cpp:406).

## 1) Protocol baseline (what the spec-level docs say)

### 1.1 Transfer fields (DPACC/APACC)

An SWD transfer is:

* **Request**: 8 bits, host-to-target
* **Turnaround (Trn)**: default 1 clock cycle (configurable in SW-DP via TURNROUND)
* **ACK**: 3 bits, target-to-host
* **Data phase**: 32 data + 1 parity (direction depends on RnW)
* Plus another **turnaround** wherever direction changes again

Near-primary sources that clearly describe this:

* NXP AN11553: describes default 1-cycle turnaround, and that the turnaround length can be changed via TURNROUND field in the SW-DP Wire Control register (wording varies by DP version) (see AN11553 PDF at https://community.nxp.com/pwmxy87654/attachments/pwmxy87654/lpc/55224/1/SWD%20Programming%20AN11553.pdf).
* Silicon Labs AN0062: also provides SWD phase diagrams and turnaround behavior (https://www.silabs.com/documents/public/application-notes/an0062.pdf).

### 1.2 Where turnarounds are required

The key “direction-change points”:

* **Always**: request (host drives) → ACK (target drives)
* **Write transfers**: ACK (target drives) → write-data (host drives)
* **Read transfers**: read-data (target drives) → idle/next-request (host drives)

Importantly:

* **Read transfers do not require a turnaround between ACK and data**, because the target continues to drive SWDIO from ACK straight into the read data phase.
* **Write transfers do require a turnaround between ACK and data**, because ownership must hand back to the host for the data phase.

These statements are explicitly shown in the phase diagrams in AN11553 and AN0062 (links above).

### 1.3 Between transactions: do we need extra clock pulses? do we need idle bits?

There are two separate concepts that get conflated:

1) **Bus can be “parked” between transactions**

* SWCLK is host-driven; it can be stopped (commonly left low) between transfers.
* SWDIO must not be driven by both sides simultaneously; outside of transfers the host typically parks it in a known state.

AN11553 states “When the bus is idle, both SWCLK and SWDIO are low” (same AN11553 link). In practice many probes leave SWDIO high when idle because the line is pulled up on many targets; this is usually tolerated, but “idle-low” is the conservative baseline.

2) **Trailing idle clocks / flush clocks (often 8 cycles of 0)**

Several SWD write-path descriptions (and many practical implementations) require **extra clocks with SWDIO=0** after a transfer to “flush”/complete certain operations.

* NXP AN11553 includes “Send at least 8 clocks with SWDIO low” as part of the connection / reset-and-switch procedure, and uses 8+ cycles of low clocking as a general “safe idle” operation (AN11553 link).
* A commonly repeated practical rule (documented in multiple community writeups and probe code) is “after a transfer, provide at least 8 idle clocks with SWDIO low”. This is frequently described as a *flush* requirement, especially after writes.

For the purposes of a **robust bit-banged implementation**, the safe recommendation is:

* Provide **trailing idle clocks with SWDIO = 0** after each transfer (or at least after writes), unless you have a reason to minimize cycles.
* After those trailing clocks, you may stop SWCLK.

## 2) What the current simulator host implementation does

### 2.1 Request preamble: two idle-low bits before every request

Both [`swd_min::dp_read()`](src/swd_min.cpp:145) and [`swd_min::dp_write()`](src/swd_min.cpp:217) insert `SWD_REQ_IDLE_LOW_BITS` low bits immediately before the 8-bit request header (default `2`):

* Macro definition: [`SWD_REQ_IDLE_LOW_BITS`](src/swd_min.cpp:152)
* Used in read: [`swd_min::dp_read()`](src/swd_min.cpp:159)
* Used in write: [`swd_min::dp_write()`](src/swd_min.cpp:224)

This is labelled “Empirical quirk (seen with ST-LINK/V2 waveforms)” in code.

**Research interpretation**: this is not generally described as a *required* spec feature. It is a valid implementation choice because extra “idle” clocks before a new request are allowed as long as they do not accidentally look like a line reset (50 consecutive 1s) or corrupt an in-progress transfer.

### 2.2 Turnaround behavior in `swd_min`

Host clocking primitives:

* One “clock period ending low”: [`swd_min::pulse_clock()`](src/swd_min.cpp:42)
* Write bit: [`swd_min::write_bit()`](src/swd_min.cpp:57)
* Read bit: [`swd_min::read_bit()`](src/swd_min.cpp:72)

#### Request → ACK turnaround

In [`swd_min::dp_read()`](src/swd_min.cpp:145) the host releases SWDIO by switching to input mode via [`swdio_input_pullup()`](src/swd_min.cpp:26) and then immediately begins reading ACK bits using [`swd_min::read_bit()`](src/swd_min.cpp:72).

Likewise in [`swd_min::dp_write()`](src/swd_min.cpp:217).

This matches the simulator’s “edge-only model” described in comments in [`swd_min::pulse_clock()`](src/swd_min.cpp:42) and implemented by the target, where the target begins presenting ACK bit 0 on a rising edge immediately after the host releases (see `TurnaroundToTarget_*` phases in [`Stm32SwdTarget::on_swclk_rising_edge()`](sim/stm32_swd_target.cpp:568)).

#### ACK → write-data turnaround

For writes, after ACK, the host does:

* `pulse_clock(); pulse_clock();` then switches SWDIO back to output (see [`swd_min::dp_write()`](src/swd_min.cpp:253)).

This is explicitly commented as “Edge-only model: target->host turnaround is 1.5 cycles” in the host.

In the target model, after the third ACK bit is presented, the target releases on a subsequent rising edge (see phases `SendAck_Write` → `TurnaroundToHost_Write` in [`Stm32SwdTarget::on_swclk_rising_edge()`](sim/stm32_swd_target.cpp:625)).

### 2.3 End-of-transfer idles are currently HIGH, not LOW

At the end of a successful DP read, the host:

1) Performs two `pulse_clock()` calls, then
2) Drives SWDIO to `1`, then
3) Calls [`line_idle_cycles(8)`](src/swd_min.cpp:104)

See: [`swd_min::dp_read()`](src/swd_min.cpp:198).

At the end of a successful DP write, the host:

* Drives SWDIO to `1`, then does [`swd_min::line_idle_cycles()`](src/swd_min.cpp:104) for 8 cycles.

See: [`swd_min::dp_write()`](src/swd_min.cpp:265).

**This is the most material divergence from many “8 idle clocks low” descriptions.**

In other words: the current code clearly *has* clock pulses after a transfer, but the “idle” state is driven HIGH (1) rather than LOW (0).

## 3) What this implies for read↔write transitions

### 3.1 Read → write

Wire-level sequence (conceptual):

```text
READ:  Req(8)  Trn  ACK(3)  Data(32+P)  Trn  Idle/next
WRITE: Req(8)  Trn  ACK(3)  Trn         Data(32+P)     Idle/next
```

Current implementation behavior:

* Ends read by returning SWDIO to host-drive HIGH and clocking 8 cycles (high), see [`swd_min::dp_read()`](src/swd_min.cpp:198).
* Starts the next write by clocking 2 “idle low” bits before request, see [`swd_min::dp_write()`](src/swd_min.cpp:224).

So the boundary has: `... idle-high clocks → idle-low clocks → request start bit`.

### 3.2 Write → read

Conceptual sequence:

```text
WRITE: Req(8)  Trn  ACK(3)  Trn  Data(32+P)  Idle/next
READ:  Req(8)  Trn  ACK(3)       Data(32+P)  Trn  Idle/next
```

Current implementation behavior:

* Ends write by driving SWDIO HIGH and clocking 8 cycles (high), see [`swd_min::dp_write()`](src/swd_min.cpp:265).
* Starts the next read with 2 idle-low bits, see [`swd_min::dp_read()`](src/swd_min.cpp:159).

## 4) Decision for this repo (clear, testable rules)

### 4.1 Host behavior we will implement

Rules:

1) **Before every transfer (read or write)**

* Keep the existing pre-request idle zeros: [`SWD_REQ_IDLE_LOW_BITS`](src/swd_min.cpp:152) defaults to `2`.
* Interpretation: this is not “required by spec”, but it is safe and matches real-world waveforms seen from some probes.

2) **After every completed transfer (read or write)**

* Drive `SWDIO = 0` and clock **8 idle cycles** ("flush/idle").
* Then stop `SWCLK` (idle low).

This means that between a read and a write you will often see:

* 8 trailing idle zeros from the previous transfer + 2 leading idle zeros before the next request = **10 zeros total**.

But we will also test with different counts (see below).

### 4.2 Target-model behavior we will implement

In [`sim::Stm32SwdTarget::on_swclk_rising_edge()`](sim/stm32_swd_target.cpp:406), during the request-collection state:

* Ignore **any number of idle zeros** before the start bit.
* Detect start-of-request on the **first `1`** after idle zeros.
* Do **not** advance the request bit index / sampling marker numbering during idle zeros.
  * Marker numbering starts at **1** on the request start bit.

This directly addresses the requirement that it must work with **any number of idle bits > 0**, not “exactly 8”.

### 4.3 Tests we will run (to prove it)

Using the standalone sims:

* [`read_then_write_simulation_main.cpp`](sim/read_then_write_simulation_main.cpp)
* [`write_then_read_simulation_main.cpp`](sim/write_then_read_simulation_main.cpp)

We will run two variants:

* **10-idle case**: keep pre-request `2` and keep post-transfer `8`.
* **2-idle case**: keep pre-request `2` and set post-transfer idle to `0` (still should work).

Acceptance:

* No SWDIO contention.
* The target continues to respond correctly.
* Marker numbering does not increase during idle zeros; it starts at 1 on the start bit.
