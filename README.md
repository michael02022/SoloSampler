# SoloSampler

A single-instrument SFZ sampler [CLAP](https://cleveraudio.org/) plugin, backed by the [sfizz](https://github.com/sfz/sfizz) SFZ engine (using the [sfizioso](https://github.com/rullopat/sfizioso) fork). The whole GUI is a self-contained [Dear ImGui](https://github.com/ocornut/imgui) window over raw X11/OpenGL2 — no JUCE, no other framework.

The core idea: SoloSampler generates a real SFZ document in memory from whatever you set in the GUI, and hands it straight to sfizz via its virtual-file loading API. Every knob/slider maps to one or more real SFZ opcodes.

## Features

- **Sample stack**: load a single sample, a multisample `.sfz`, or several files at once (drag & drop, or multi-select in the file dialog) — everything in the stack plays **simultaneously, layered** on every note (not velocity-switched or round-robin). Drag & drop and click-to-browse both supported; waveform view with loop-start/loop-end/offset markers when the stack is a single plain sample.
- **Sample tab**: Bend Range (200/1200/2400/4800 cents), MPE toggle, Quality, Loop Mode, Polyphony/Note Polyphony, Volume/Pan (with live CC7/CC10 send), Character (transpose a whole imported multisample mapping), sample start Offset with an optional randomized component.
- **Pan tab**: hard-pan "x2" mode, Pan Random (+ Alternate CC slot), Pan LFO.
- **Amp tab**: Keycenter/Keytrack/Veltrack/Random, a full amp envelope (Delay/Attack/Hold/Decay/Sustain/Release + curve shapes), Amp LFO, and velocity-to-attack/sustain/volume modulation depths.
- **Filter tab**: 23 SFZ v2/ARIA filter types, Cutoff/Resonance/Random Cutoff/Keytrack/Veltrack, an optional filter envelope, and a Filter LFO.
- **Pitch tab**: Keytrack/Veltrack/Random, Portamento/Glide, an optional pitch envelope, a Pitch LFO, and **Scala (.scl) tuning** support with an adjustable tuning-frequency (A4) reference.
- **Opcodes tab**: a free-form text box — anything typed there is written verbatim into the generated SFZ, for any opcode the UI doesn't expose.
- **FX tab**: an SFZ-native "Opcode FX" chorus/unison emulation (Unison, Chorus Mono, Chorus Stereo Wet / Wet+Dry, with its own filter and CC-modulatable detune/delay/phase), plus a real DSP reverb (sfizz's built-in `<effect> type=fverb`, 7 room types).
- **Log tab**: a live MIDI monitor and a read-only view of the exact SFZ text currently loaded into the engine.
- **Presets**: `.sspreset` (the whole instrument, including the loaded sample/stack) and `.ssprofile` (every design parameter *except* the loaded sample — reusable across different source material) via native `zenity` file dialogs, defaulting to `~/SoloSamplerPresets/`.
- **Export SFZ**: writes the currently-generated SFZ out as a standalone, portable `.sfz` file with real absolute sample paths.
- On-screen 128-key piano (left-click previews, right-click sets root note), live voice counter, "Reset to Default" (with confirmation).

## Requirements

- Linux with X11 (the GUI is X11 + OpenGL2 — no Windows/macOS support currently)
- CMake ≥ 3.16 and a C++20 compiler (g++/clang++)
- `pkg-config`, `libsndfile`, X11 development headers, OpenGL development headers, POSIX threads
- `xxd` (embeds the Font Awesome icon font into the binary at build time)
- `zenity` at runtime (native file dialogs for Save/Load Preset/Profile and Export SFZ)

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libsndfile1-dev libx11-dev libgl1-mesa-dev xxd zenity
```

The vendored sfizz shared library (`lib/libsfizz.so.1.2.3`) and its headers, the CLAP headers (`external/clap/`), Dear ImGui (`external/imgui/`), and the Font Awesome font (`external/fontawesome/`) all ship inside this repository — no submodules to fetch, no extra download step.

## Building

```sh
cmake -B build
cmake --build build -j$(nproc)
```

This produces `build/SoloSampler.clap`.

## Installing

Copy (or symlink) the built plugin into your CLAP plugin directory:

```sh
mkdir -p ~/.clap
cp build/SoloSampler.clap ~/.clap/
```

Most CLAP hosts on Linux also scan `/usr/lib/clap/` and `/usr/local/lib/clap/` system-wide.

## Testing

A standalone headless test host is included — it `dlopen`s the built `.clap` and drives it without a real DAW:

```sh
./build/mini_host build/SoloSampler.clap [sample.wav] [gui]
```

Passing a sample path exercises real audio rendering/routing checks; adding `gui` on the end opens the actual plugin window (useful for visual verification, since it's the real GUI code running against the real plugin).

## Project layout

```
src/
  plugin.cpp          CLAP entry point, descriptor, state save/load, preset I/O
  shared.hpp           GUI-thread ↔ audio-thread shared parameter struct
  gui/                 Dear ImGui widget layer, X11 window/GL context, file dialogs
  sfizz/                Thin RAII wrapper around the sfizz C API
  state/                SFZ text generation, sample analysis, multisample flattening, preset files
lib/                    Vendored sfizz shared library + headers
external/               Vendored CLAP headers, Dear ImGui, Font Awesome
test/                   mini_host standalone test harness
```

## Third-party components

| Component | Location | License |
|---|---|---|
| [sfizz](https://github.com/sfz/sfizz) (sfizioso fork) | `lib/` | BSD-2-Clause |
| [CLAP](https://github.com/free-audio/clap) | `external/clap/` | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | `external/imgui/` | MIT |
| Font Awesome 4 | `external/fontawesome/` | SIL OFL 1.1 (font), zlib (header) |

## License

This project's own source code doesn't declare a license yet — add one (e.g. MIT) before distributing it publicly if that matters for your use case.
