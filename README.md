# MeshCoreTRX

A full-duplex **MeshCore** transceiver for the **ADALM-PLUTO (PlutoSDR)**, with
a Dear ImGui desktop UI and a small command-line transmitter.

Default radio settings: **910.525 MHz**, **62.5 kHz**, **SF7**, **CR8**,
sync word **0x12**, MeshCore **Public** channel.

## Features

- PlutoSDR full-duplex RX/TX
- LoRa PHY modulator and demodulator (preamble, sync, SFD, header, FEC,
  whitening, CRC)
- MeshCore adverts, group text/data, direct messages, ACK and path learning
- Companion and Repeater roles with forwarding and deduplication
- GUI: dashboard, chat, contacts, packet log, settings
- Persistent identity, contacts, channels and chat history

## Build

Windows + MSYS2 MinGW-w64:

```powershell
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake `
    mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf `
    mingw-w64-x86_64-glfw mingw-w64-x86_64-libiio

cd MeshCoreTRX
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## Run

```powershell
.\MeshCoreTRX.exe --start --uri usb:1.8.5
```

Or open the GUI and press **Start radio**. The chat box auto-focuses: type and
press Enter to send.

If the Pluto's USB path changes (for example `usb:1.8.5` becomes `usb:1.17.5`),
the app automatically scans and uses the first Pluto it finds. `ip:192.168.2.1`
also works.

Command-line transmitter:

```powershell
.\MeshCoreTRX-cli.exe --uri usb:1.8.5 --mode group --advert `
  --name MeshCoreTRX --text "Hello from MeshCoreTRX"
```

Useful GUI options:

```text
--start                 start radio immediately
--uri URI               usb:1.8.5 or ip:192.168.2.1
--freq MHZ --bw KHZ --sf N --cr N
--atten DB --rxgain DB
--role companion|repeater
--name NAME --lat DEG --lon DEG
--send TEXT --advert --exit-after SECONDS
```

## Tests

```powershell
.\build\MeshCoreTRX-prototest.exe      # protocol + ACK + repeater forwarding
.\build\MeshCoreTRX-persisttest.exe    # persisted contacts/channels/messages
.\build\MeshCoreTRX-rxtest.exe usb:1.8.5 910.525 40 0
.\build\MeshCoreTRX-radiotest.exe usb:1.8.5 910.525 40 0
```

The Pluto runs at 3.84 MSPS and the modem at 250 kSPS. Native Pluto TX power is
low; use an antenna on both ports and a PA for range. Full-duplex operation
benefits from separate RX/TX antennas or a duplexer.

Third-party attribution is in [NOTICE.md](NOTICE.md). Licensed under MIT.
