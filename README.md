# AudioXtreamer
A simple multichannel USB/FPGA PCM audio interface:

## Current Windows milestone (August 2026)

The project now builds on Windows with Visual Studio 2022 (using the v142
toolset), Steinberg ASIO SDK 2.3.4, WiX 3, and Xilinx ISE 14.7. A generated
Spartan-6 image and the 64-bit Windows application/ASIO driver have been tested
with a ZTEX 2.01 and Yamaha 01X at 44.1 kHz. Multichannel playback is working.

This milestone adds:

- a persistent in-application log with device, ASIO, USB, FIFO, sample-rate,
  and anomaly reporting;
- clear connected/running/ASIO-client status indicators;
- safer device reopen and single-application-instance handling;
- corrected 32-bit and 64-bit ASIO/COM registration in the x64 MSI;
- an FPGA interface-v4 output mute with host readback verification, plus a
  software gain ramp around stream reconfiguration;
- USB packet/framing, feedback-queue, cleanup, counter, and FPGA-loader fixes;
- ASIO block choices restricted to powers of two from 16 through 1024;
- FPGA output-FIFO choices restricted to multiples of 16 from 16 through 240,
  with 512/64 used as the current conservative test configuration; and
- an ISE placement setting that completed place-and-route with timing score 0.

**Known limitation:** audio is not yet glitch-free. Logs still show USB capture
errors, FPGA output refills, FIFO excursions, and occasional host resync events;
audible clicks have also occurred. A 512-sample ASIO block is the repeatable
baseline; 1024 samples has produced fewer glitches so far, but neither setting
has established glitch-free operation. Testing so far used a Dell USB-C dock,
and direct-USB testing is still pending. Do not treat this milestone as a
production audio release.

See [the USB audio stability handover](docs/USB_AUDIO_STABILITY_HANDOVER.md) for
the investigation, build requirements, verified behavior, and remaining work.

<p align="center">
<img src="/images/1V2A0353.jpg" alt="PCB" width="600" class="center" >
</p>



Initially started as a simple way to add usb connectivity to the Yamaha 01x firewire based digital mixer & control surface, this project attempts to provide a simple low latency ASIO interface to a usb connected fpga that handles the i2s I/O.

Inspired by [Koon](https://sites.google.com/site/koonaudioprojects/usb-to-multi-channel-usb2-0) and his 24ch out usb interface, it builds further by providing 32in/32out on a simple usb2.0 interface and bypassing any vendor specific driver to communicate with the lowest possible latency with the fpga.

The proof of concept was built and written around the ft232h usb fifo in combination with a zynq fpga but it rapidly became clear that from a practical point of view, a more flexible Cypress FX2LP and a Xilinx Spartan 6 would suffice the needs of the interface. Although the code is now fully adapted to the 16bit data bus of the Cypress and the CoreGenerator fifos of ISE, it is easily portable to the original 8 bit FTDI and the new Vivado IP generator.

The AudioXtreamer can be seen as 3 different parts:
  - A generic C++ ASIO (TortugASIO) multi-threaded 32/64 bit driver that bridges the audio client/DAW to the USB backend
  - AudioXtreamer is a simple user mode driver that uses isochronous transfers to communicate with the fx2lp, plus a tray UI to          configure asio buffer sizes, channel count and midi ports. It uses USBDk/WinUSB as backend layer to the FX2LP.
  - VHDL fpga code for the Spartan 6 which handles the usb fifos, sample buffer and generation and decoding of the PCM I/O
  
The sources are written in such a way that the user can configure how many inputs and outputs can be handled in software and hardware.
 
To build the whole project you will need basic knowledge of C/C++ and Visual Studio, VHDL and ISE plus basic digital electronics skills.
 
The hardware implementation is based on the [ZTEX 2.01](https://www.ztex.de/usb-fpga-2/usb-fpga-2.01.e.html) but simple Aliexpress fx2lp / spartan6 parts should also work with minimal effort.


Xtreamer and ZTEX module for 01x |  PCB
:-------------------------:|:-------------------------:
 <img src="/images/1V2A0354.jpg" alt="PCB" width="500"> | <img src="/Pcb/AudioXtreamer_Ymh01x.png" alt="Gerber" width="500">
  
  <p align="center">
  Yamaha 01x retrofitted with AudioXtreamer (USB/ADAT)

  <img src="/images/1V2A0355.jpg" alt="PCB" width="800">
</p>
