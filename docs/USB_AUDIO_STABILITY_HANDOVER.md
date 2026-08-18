# AudioXtreamer USB Audio Stability Handover

Date: 2026-08-18

## Purpose and status

This handover records the source-level investigation into unstable/glitchy USB
audio, the fixes made in this change set, the limits of what has been proven,
and the Windows/FPGA validation work still required.

The changes have been reviewed statically and the host packet-distribution
algorithm has been exercised with a portable model. They have **not** yet been
compiled with Visual Studio, synthesized with Xilinx ISE, or run on the Yamaha
01X hardware. Hardware validation on Windows is the next required step.

## Critical channel-count clarification

The Yamaha 01X service manual gives asymmetric mLAN Audio I/F counts, stated
from the mixer's point of view:

- 18 input / 24 output channels at 44.1/48 kHz.
- 10 input / 16 output channels at 88.2/96 kHz.

The input side is 16/8 patchable mLAN mixer inputs plus a dedicated two-channel
mLAN monitor input. The output side supports 24/16 direct outputs. See the
General Specifications on page 3 of
[`Yamaha-01X-Service-Manual.pdf`](Yamaha-01X-Service-Manual.pdf#page=3).

From the Windows/ASIO host's point of view, the directions reverse:

- **24 ASIO inputs / 18 ASIO outputs** at 44.1/48 kHz.
- **16 ASIO inputs / 10 ASIO outputs** at 88.2/96 kHz.

The FPGA top level physically exposes 12 Yamaha-facing serial inputs and 12
serial outputs (`ain(11:0)` and `aout(11:0)`) and the PCM decoder represents two
24-bit slots per serial line. That is a physical capacity of 24 slots in each
direction. It matches the 24-channel 01X-to-host base-rate path, while only nine
lines are needed for the 18-channel host-to-01X path. See
[`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L45) and
[`pcmio.vhd`](../VHDL/usb2iis/pcmio.vhd#L28).

The manual's block diagram on page 135 independently states Tx Digital Audio as
24 channels maximum, Rx Digital Audio as 16 channels, and the physical format as
24-bit, MSB-first, left-justified, two channels per line. DM schematic page 138
shows `MLAN-OUT[1..12]` and `MLAN-IN[1..12]` on the board connector. MLN2
schematic page 151 shows the corresponding `AIN1..12` and `AOUT1..12` signals.

The Windows settings are now capped at 24 ASIO inputs (12 stereo-line entries)
and 18 ASIO outputs (nine stereo-line entries) in
[`ASIOSettings.cpp`](../AudioXtreamer/AudioXtreamer/ASIOSettings.cpp#L13),
[`ASIOSettings.h`](../AudioXtreamer/AudioXtreamer/ASIOSettings.h#L38), and
[`SettingsDlg.cpp`](../AudioXtreamer/AudioXtreamer/SettingsDlg.cpp#L148).

Automatic restriction to 16 ASIO inputs / 10 ASIO outputs at 88.2/96 kHz has
**not** been added. The supported totals are now established, but the exact
double-rate line/slot ordering, including the dedicated monitor pair, must still
be confirmed before silently changing a running configuration. Manually select
16 inputs and 10 outputs for double-rate tests.

### Service-manual connector evidence

The MLN2 schematic's generic `mLAN2-I/F` connector table on service-manual page
151 gives these audio-related row assignments (each row has an `a` and `b`
contact):

| Row | MLN2-side names | Function |
| --- | --- | --- |
| 1 | `GND` / `GND` | Digital ground |
| 2 | `WCKI` / `WCKO` | Word clock into/out of MLN2 |
| 3 | `BCKI` / `BCKO` | Bit clock into/out of MLN2 |
| 4 | `MCKI` / `MCKO` | Master clock into/out of MLN2 |
| 5 | `DETECT1` / `DETECT2` | Board/interface detection |
| 6-13 | `AIN1..8` / `AOUT1..8` | Eight serial audio lines each way |
| 14 | `GND` / `GND` | Digital ground between audio groups |
| 15-18 | `AIN9..12` / `AOUT9..12` | Four more serial audio lines each way |
| 19-22 | `AIN13..16/M5..8` / `AOUT13..16/M5..8` | Generic MLN2 audio/MIDI multiplexed pins |
| 23 | `GND` / `GND` | Digital ground |

The 01X DM schematic on page 138 is the product-specific authority: it routes
`MLAN-OUT[1..12]` and `MLAN-IN[1..12]` on rows 6-18. Row 19 is used for MIDI port
5 and the remaining generic audio-13..16 positions are not used as additional
01X audio lines. This agrees with the replacement PCB and FPGA top level having
12 data lines each way, rather than 16.

The same page shows the 01X DM side driving the MLN2 clock-input contacts and
accepting `MLAN-WCKO` on the return word-clock contact; the MLN2 bit/master-clock
output contacts are NC in this product. This is consistent with the 01X being
able to slave its internal clock system to mLAN while still generating the local
PCM bit/master clock used by its DSP-side serial interface. The current FPGA top
level receives both `pcm_clk` and `word_clk` from the 01X connector and does not
generate either one.

## Runtime architecture

```text
Yamaha 01X PCM pins
    | 12 serial inputs, 12 serial outputs, PCM clock, word clock
    v
pcmio (clkpcm domain)
    | two 24-bit slots per serial line
    v
72-bit asynchronous FIFOs (clkpcm <-> 48 MHz USB/FX2 domain)
    | arrays of 24-bit channels
    v
isoch_audio_in / isoch_audio_out
    | packed 16-bit FX2 FIFO words, 3 words per stereo pair
    v
s_axis_to_w_fifo (FX2 slave FIFO bus)
    | EP6 IN / EP2 OUT isochronous transfers
    v
CypressDevice Windows worker (WinUSB)
    | 24-bit interleaved PCM in shared memory
    v
AudioXtreamer process <-> TortugASIO process
    | named shared mapping and events
    v
ASIO client
```

Key source points:

- Yamaha pins and top-level wiring: [`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L45), lines 45-51 and 167-170.
- PCM serializer/deserializer: [`pcmio.vhd`](../VHDL/usb2iis/pcmio.vhd#L19).
- Clock-domain FIFOs: [`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L418), lines 418-493.
- USB OUT unpacker: [`isoch_audio_out.vhd`](../VHDL/usb2iis/isoch_audio_out.vhd#L22).
- USB IN packer: [`isoch_audio_in.vhd`](../VHDL/usb2iis/isoch_audio_in.vhd#L18).
- FX2 FIFO bridge: [`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L559).
- Windows USB worker: [`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L333).
- Process-to-process shared memory: [`AudioXtreamer.cpp`](../AudioXtreamer/AudioXtreamer/AudioXtreamer.cpp#L48) and [`AudioXtreamerDevice.cpp`](../AudioXtreamer/TortugASIO/AudioXtreamerDevice.cpp#L44).

## Confirmed defects and fixes

### 1. FPGA USB IN violated stream backpressure

`isoch_audio_in` could read the next asynchronous-FIFO sample when the current
sample was complete even if `m_axis_tready` was low. It also updated packet word
state independently of a successful valid/ready transfer. The FX2 interface is
half duplex and can stall this path, so the old behavior could drop, duplicate,
or rearrange samples under contention.

The FIFO read now occurs only when no output word is pending or when the final
word of the current sample is accepted. `tvalid`, the word counter, and packet
termination now advance only on a valid/ready handshake. See
[`isoch_audio_in.vhd`](../VHDL/usb2iis/isoch_audio_in.vhd#L68), lines 68-114.

The `nr_ins` range was also corrected so the configured maximum number of
physical serial lines is representable (line 50).

### 2. USB OUT feedback state could be overwritten

Each USB IN completion supplies the measured number of samples in each of 16
high-speed USB microframes. The Windows code uses that distribution as implicit
feedback for the next USB OUT transfer.

Previously there was one global mutable `spp[16]` array and one accumulated
sample count. A second RX completion could overwrite the distribution before TX
used it, while TX itself destructively reduced the same array. This loses the
temporal relationship between input and output transfers.

The code now stores complete 16-microframe distributions in an eight-entry FIFO
and TX consumes the oldest distribution. Initialization is at
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L454), queue use is
at lines 626-647 and 745-833, and storage is declared in
[`CypressDevice.h`](../AudioXtreamer/FX2LP/CypressDevice.h#L28).

This prevents overwrite; it does not smooth the measured feedback. Commit
`deca269` in the existing history already describes the feedback as too jittery
and says smoothing is necessary.

### 3. ASIO block sizes of 256/512/1024 wrapped to zero

`DistributeSamples` accepted the ASIO sample count as `uint8_t`. A configured
block size of 256, 512, or 1024 therefore truncated to zero when passed to the
function, preventing correct USB OUT copying and encouraging underruns/silence.

Sample counts and arithmetic are now `uint32_t`. See
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L593), lines
593-618, and its caller at lines 653-669.

### 4. Empty/full WinUSB microframes were parsed as audio

WinUSB submits fixed 1024-byte microframe buffers. The FPGA requires the marker
`0xaa5555aa` to distinguish the logical PCM payload from the zero-filled tail.
The old code did not reliably write a marker when no feedback or no ASIO samples
were available. The FPGA could therefore parse the entire zero-filled packet as
valid silence samples, rapidly overfilling or phase-shifting its output FIFO.

Every microframe now gets a marker immediately after its intended sample count,
including all-silence packets. The sample count is clamped so the marker always
fits within 1024 bytes. See
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L620), lines
620-680.

The FPGA marker detector now only starts a marker candidate at a complete audio
frame boundary (`rx_state = w0`, `outs_counter = 0`). This greatly reduces false
termination when ordinary PCM happens to contain one marker word. See
[`isoch_audio_out.vhd`](../VHDL/usb2iis/isoch_audio_out.vhd#L56), lines 56-83.
The 32-bit marker can still theoretically collide with PCM at that exact frame
boundary; the protocol is not collision-proof.

The output line counter ranges were corrected so the configured maximum number
of physical serial lines is representable (line 56).

### 5. Invalid INI values could escape UI limits

The settings UI bounded values, but values loaded directly from the INI file
were accepted without validation. This could configure channel counts beyond
the synthesized FPGA dimensions or invalid block/FIFO sizes.

Loaded settings are now reset to defaults when outside their declared ranges.
See [`ASIOSettingsFile.cpp`](../AudioXtreamer/AudioXtreamer/ASIOSettingsFile.cpp#L56),
lines 56-70.

### 6. Input ASIO buffer was zeroed using the output channel count

With unequal input/output counts, `createBuffers` allocated by `mNumInputs` but
zeroed by `mNumOutputs`, causing incomplete initialization or a write beyond the
input allocation. It now uses `mNumInputs`. See
[`TortugASIO.cpp`](../AudioXtreamer/TortugASIO/TortugASIO.cpp#L500), lines
500-518.

### 7. ASIO and WinUSB cleanup used incorrect delete/free semantics

The ASIO backing sample blocks were leaked, and several arrays were released
with scalar `delete`. The backing blocks and pointer/map arrays are now released
with `delete[]`. See [`TortugASIO.cpp`](../AudioXtreamer/TortugASIO/TortugASIO.cpp#L565),
lines 565-586.

WinUSB cleanup now calls `WinUsb_Free` before closing the device handle, uses
`delete[]` for descriptor arrays, deletes the actual context object, and clears
the context pointer. See
[`WinUSBHelper.cpp`](../AudioXtreamer/WinUSB/WinUSBHelper.cpp#L258), lines
258-275, and lines 453-478.

These are lifecycle fixes rather than likely steady-state click sources, but
they remove definite undefined behavior and reopen/close instability.

### 8. A normal worker timeout stopped streaming

`WaitForMultipleObjects` uses a 200 ms timeout. The old switch treated
`WAIT_TIMEOUT` as an unexpected failure and stopped the worker. It is now logged
and the loop continues. See
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L504), lines
504-540.

### 9. Existing FPGA error counters were not read by Windows

FPGA status register 2 contains sample-rate count and output FIFO level; register
3 contains output-skip and input-full counters. See
[`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L196), lines
196-203.

Windows previously polled only register 2. It now alternates register 2 and 3,
so the existing UI Skip and Full diagnostics receive real values. Failed EP6 IN
isochronous packet descriptors also increment `Ep6IsoErr`. See
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L685), lines
685-741 and 745-833.

## USB packet and PCM format

- USB high-speed microframe payload capacity is fixed at 1024 bytes in the host
  code. Each transfer contains 16 microframes, hence a nominal 2 ms transfer.
  Two transfers per direction are allocated. See
  [`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L333), lines
  333-341.
- PCM in shared memory and USB payloads is packed 24-bit, interleaved by channel,
  with no per-sample header. Host strides are `channels * 3` bytes at lines
  363-372.
- The FPGA converts each pair of 24-bit channels to/from three 16-bit FX2 words.
  USB IN packing is at
  [`isoch_audio_in.vhd`](../VHDL/usb2iis/isoch_audio_in.vhd#L117), lines
  117-145. USB OUT unpacking is at
  [`isoch_audio_out.vhd`](../VHDL/usb2iis/isoch_audio_out.vhd#L156), lines
  156-177.
- USB IN packet termination uses AXI-style `tlast` and `PKTEND`. USB OUT cannot
  communicate per-microframe logical lengths through the current WinUSB path, so
  the host inserts the `0xaa5555aa` marker in the fixed buffer.
- Output packet pacing mirrors the sample count measured in USB IN microframes.
  There is no explicit USB Audio Class feedback endpoint in this Windows stream
  path and no FIFO-level control loop.

## Buffering and scheduling

FPGA buffering:

- Yamaha-to-USB: PCM samples are captured in the Yamaha `clkpcm` domain and
  cross to the 48 MHz USB domain through 72-bit x 256 asynchronous FIFOs.
- USB-to-Yamaha: samples cross from the 48 MHz USB domain to `clkpcm` through
  equivalent asynchronous FIFOs. Output starts only after the programmable-full
  threshold is reached and returns to fill mode after empty. See
  [`top_audioxtreamer.vhd`](../VHDL/usb2iis/top_audioxtreamer.vhd#L383), lines
  383-447.

Windows buffering:

- 16 logical ASIO blocks are indexed in the shared-memory ring. See
  [`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L340), lines
  340-341 and 389-400.
- A single MMCSS `Pro Audio` worker handles timer, EP0 status, ASIO IPC, USB IN,
  and USB OUT events. See lines 347-360 and 504-540.
- AudioXtreamer and TortugASIO are separate processes linked by a named shared
  memory mapping and named events. The application publishes offsets/strides at
  [`AudioXtreamer.cpp`](../AudioXtreamer/AudioXtreamer/AudioXtreamer.cpp#L90),
  and the ASIO-side copy loops are in
  [`TortugASIO.cpp`](../AudioXtreamer/TortugASIO/TortugASIO.cpp#L135).

## Remaining risks and unresolved work

These items are not fixed by this change set:

1. **No automatic high-rate channel policy.** The service manual establishes 16
   ASIO inputs and 10 ASIO outputs at 88.2/96 kHz, but exact line/slot ordering
   and monitor-pair placement still require verification before auto-remapping.
2. **No feedback smoothing.** TX follows individual RX packet sample counts. A
   queue preserves order but does not remove host scheduling jitter or regulate
   the FPGA output FIFO around a target level.
3. **Small USB request queue.** Two 2 ms requests provide roughly 4 ms of
   submitted buffering per direction. That may be marginal on a loaded Windows
   machine.
4. **Completion-order assumption.** The worker waits on only the current request
   index for each direction and assumes requests complete in submission order.
5. **Limited WinUSB error handling.** Several submission return values are not
   acted on, and transfer-level completion is not consistently confirmed with
   `WinUsb_GetOverlappedResult` before descriptor inspection/resubmission.
6. **ASIO ring-full handling.** `RxIsochCB` logs `ASIO queue full!`, but the ring
   ownership/recovery behavior needs a stress test and likely a deliberate drop
   policy.
7. **Marker protocol collision.** Boundary qualification makes collision much
   less likely but a PCM frame beginning with the two marker words is still
   indistinguishable from EOF.
8. **CDC/status review.** Audio data uses generated asynchronous FIFOs, but status
   counters and resets crossing domains need dedicated simulation and timing
   review.
9. **FX2 firmware absent.** The repository contains host calls and FPGA logic but
   not the Cypress FX2 firmware/USB descriptors. Endpoint setup, alternate
   settings, and SOF behavior cannot be audited end-to-end from this repository.
10. **No platform build in this pass.** macOS lacks the Windows SDK/Visual Studio
    and Xilinx ISE toolchain, so C++ compilation and FPGA synthesis remain open.

## Windows checkout and prerequisites

From the repository checkout:

```powershell
git pull origin master
git submodule update --init --recursive
```

The submodule command is required for `libs/simpleini`, referenced by the Visual
Studio project.

### Required development tools

Visual Studio:

- A modern Visual Studio installation is suitable; Visual Studio 2022 does not
  need to be replaced with the 2019 IDE.
- In Visual Studio Installer, add the **Desktop development with C++** workload.
- Under Individual components, install **MSVC v142 VS 2019 C++ x64/x86 build
  tools**. The checked-in projects explicitly select `v142`; do not retarget to
  `v143` until a known-good baseline has been built and tested.
- Install a **Windows 10 SDK**. The projects request version `10.0` without
  pinning a minor SDK revision.
- Install **C++ MFC for v142 x86/x64**. `AudioXtreamer.vcxproj` builds an MFC
  application and uses static MFC in Debug and dynamic MFC in Release.
- Install **C++ ATL for v142 x86/x64** because the application includes ATL
  headers. Spectre-mitigated libraries are not required by the current project.

External source/tool dependencies:

- **Steinberg ASIO SDK 2.3** is required to compile `TortugASIO`. It is not in
  this repository. Place it at repository-root `asiosdk2.3/` so that
  `asiosdk2.3/common/asiosys.h` and `asiosdk2.3/common/iasiodrv.h` exist, matching
  the include path in `TortugASIO.vcxproj`. Observe Steinberg's SDK licence.
- **WiX Toolset v3.11 or newer in the v3 line** is required only for the MSI
  project. Install the WiX v3 build tools and the Visual Studio extension if the
  installer project must load inside the IDE. The checked-in `.wixproj` imports
  `Microsoft\WiX\v3.x\Wix.targets`; WiX v4/v5 alone is not a drop-in replacement.
  The EXE and ASIO DLL can be built first with the WiX project unloaded.
- **Xilinx ISE Design Suite 14.7** with Spartan-6 device support and Core
  Generator is required for the FPGA. Vivado does not support the Spartan-6
  XC6SLX16 target used here. ISim supplied with ISE is sufficient for the
  existing testbenches.
- **Git for Windows** is needed if it is not already installed. It is also used
  to initialize `libs/simpleini` with the submodule command above.

Device/runtime prerequisites:

- The active host backend is WinUSB. The Windows SDK supplies `winusb.h` and
  `winusb.lib`; no custom kernel driver is built by this solution.
- A clean machine still needs the existing FX2 device/interface associated with
  the WinUSB driver. There is no driver INF or FX2 firmware in this repository,
  so record the currently working Device Manager driver binding before changing
  it. UsbDK source is present but its backend exports are disabled.
- The FX2 must already contain compatible firmware implementing the expected
  endpoints, alternate setting 3, ZTEX FPGA-load requests, LSI register access,
  and SOF signal. ISE builds only the FPGA image, not FX2 firmware.

### Windows software build

- Open `AudioXtreamer/AudioXtreamer/AudioXtreamer.sln`.
- The projects target Visual Studio 2019 toolset `v142` and Windows 10 SDK.
- Both Win32 and x64 configurations exist. Test the architecture used by the
  intended ASIO host; 64-bit hosts require the x64 driver/build.
- Build the `AudioXtreamer` and `TortugASIO` projects first. Build the WiX
  installer afterward so the ASIO COM registration is applied consistently.

### FPGA build

- Open `FPGA/ZTEX201/USB32chAudio/USB32chAudio.xise` in Xilinx ISE 14.7.
- Target device is Spartan-6 `xc6slx16-ftg256-2`.
- Run synthesis, implementation, and Generate Programming File.
- The project property `Create Binary Configuration File` is enabled and output
  name is `top_audioxtreamer`, so the expected result is
  `FPGA/ZTEX201/USB32chAudio/top_audioxtreamer.bin`.

**Build-order warning:** `top_audioxtreamer.bin` is generated and is currently
absent from the repository. The Windows resource script embeds it as
`IDR_FPGA_BIN`; see
[`AudioXtreamer.rc`](../AudioXtreamer/AudioXtreamer/AudioXtreamer.rc#L274) and
[`AudioXtreamer.vcxproj`](../AudioXtreamer/AudioXtreamer/AudioXtreamer.vcxproj#L252).
Generate/copy the correct FPGA binary before building the Windows application.
`CypressDevice` loads that resource, bit-reverses each byte, and programs the
FPGA over EP0 at
[`CypressDevice.cpp`](../AudioXtreamer/FX2LP/CypressDevice.cpp#L42).

Do not validate the host fixes with an old FPGA binary: the backpressure and EOF
changes are in VHDL and must be present in the image embedded in the executable.

## Initial hardware validation matrix

Use a repeatable routed signal (tone plus periodic impulse or channel-number
pattern) so channel order, duplication, and loss are visible as well as audible.
Record the selected mode, channel count, ASIO block size, FIFO setting, duration,
and all counters for each run.

### Phase A: basic operation

1. Build the new FPGA binary, then build/install the matching Windows software.
2. Start at 48 kHz, 24 ASIO inputs / 18 ASIO outputs, 256-sample ASIO blocks,
   FIFO 64.
3. Confirm hardware sample rate and software sample rate agree.
4. Confirm all 24 input and 18 output channels are unique and in the expected
   order.
5. Run for at least 30 minutes while watching Skip and Full counters.

### Phase B: supported rates and block sizes

Run 44.1 and 48 kHz at 24 ASIO inputs / 18 ASIO outputs with block sizes 64,
128, 256, 512, and 1024. The 256/512/1024 cases specifically verify the former
`uint8_t` overflow.

Run 88.2 and 96 kHz with the application manually configured for **16 ASIO
inputs / 10 ASIO outputs**. Do not test or advertise the base-rate 24/18 counts
at double rates as supported behavior.

### Phase C: recovery and load

- Repeatedly open/close the control application and ASIO client.
- Start/stop streaming and change sample rate between sessions.
- Test input-only use, output-only use at the ASIO client, and unequal channel
  selections where the client permits them.
- Add realistic CPU/DPC load and run the smallest practical ASIO block.
- Check for worker timeout messages, `ASIO queue full!`, endpoint errors,
  ResyncErrors, Skip, and Full increments.

Pass criteria after startup:

- No audible clicks, repeats, channel swaps, or bursts of silence.
- No continuing output Skip or input Full counter increments.
- No endpoint/resync errors.
- Stable reported sample rate and FIFO level rather than monotonic drift to empty
  or full.
- Clean restart without process, heap, or device-handle failures.

## Recommended next engineering steps

1. Complete the Windows/ISE build and the validation matrix above before making
   further architectural changes.
2. Measure and document the exact 88.2/96 kHz physical slot mapping, then enforce
   24-in/18-out at base rates and 16-in/10-out at double rates in one shared
   host-perspective policy.
3. Add a proper output-rate controller using measured Yamaha sample rate and
   FPGA FIFO level. Use filtering/hysteresis rather than mirroring one USB IN
   microframe at a time.
4. Increase and generalize the isochronous request ring, and process any completed
   request rather than only the expected index.
5. Check every WinUSB submission/completion result and define recovery for failed
   packets, queue overflow, and ASIO ring overrun.
6. Add VHDL tests that hold `tready` low at every word position, exercise FIFO
   empty/full transitions, verify the 512-word packet limit, and test marker-like
   PCM values at and away from frame boundaries.
7. Obtain the FX2 firmware/descriptors and include them in the end-to-end audit.

## Files changed in this stability pass

- `AudioXtreamer/AudioXtreamer/ASIOSettings.cpp`
- `AudioXtreamer/AudioXtreamer/ASIOSettings.h`
- `AudioXtreamer/AudioXtreamer/ASIOSettingsFile.cpp`
- `AudioXtreamer/AudioXtreamer/SettingsDlg.cpp`
- `AudioXtreamer/FX2LP/CypressDevice.cpp`
- `AudioXtreamer/FX2LP/CypressDevice.h`
- `AudioXtreamer/TortugASIO/TortugASIO.cpp`
- `AudioXtreamer/WinUSB/WinUSBHelper.cpp`
- `VHDL/usb2iis/isoch_audio_in.vhd`
- `VHDL/usb2iis/isoch_audio_out.vhd`
- `docs/USB_AUDIO_STABILITY_HANDOVER.md`
- `docs/Yamaha-01X-Service-Manual.pdf` (source material supplied for this review)

No PCB files, FX2 firmware, or Yamaha PCM channel mapping were changed.
