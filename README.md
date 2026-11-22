# mk1-shell

Firmware for mk-1, a USB-C business card built around an RP2350 and a 16 × 9 LED matrix. Connect a terminal to program a fictional chip in assembly. The manual has missing pieces, and the shell gives you a way to test what each instruction does.

**[Open mk-1](https://keetsta.me/mk1/)** · [Firmware releases](https://github.com/keetsta/mk1-shell/releases) · [Leaderboard](https://keetsta.me/mk1/top/)

<picture>
  <source media="(max-width: 600px)" srcset="docs/media/mk1-shell-narrow.webp">
  <img src="docs/media/mk1-shell.webp" alt="The mk-1 business card and its 16 × 9 LED matrix">
</picture>

## Try one instruction

Choose **try it here** on the mk-1 site, or open a terminal on your card. At the mk-1 prompt, enter these two commands.

```text
try mov 42 a
regs
```

`try` executes one instruction in the shell's scratch state. The first command sets register `a` to `42`, and `regs` reads that state back. Use `man` to read the manual, `tasks` to find a puzzle, and `help` for the command list.

## The matrix on the card

Programs can draw on the card's 144 LEDs and read its button and light sensor. An IS31FL3731 drives the matrix, and the firmware adjusts brightness to ambient light.

<picture>
  <source media="(prefers-reduced-motion: reduce)" srcset="docs/media/matrix-in-motion.webp">
  <img src="docs/media/matrix-in-motion.gif" alt="Simulated mk-1 matrix showing boot, standby, and a screensaver">
</picture>

The simulator shows power-on and standby on the left, with the cube screensaver on the right.

When the main firmware is idle, press the button to show the site address. The card starts a screensaver after five minutes of inactivity and sleeps after an hour. Ctrl+D at an empty prompt returns the matrix to the card. A running program keeps control until it stops, even if you close the terminal.

## One core, three runtimes

The assembler, virtual chip, puzzles, and shell live in [`src/core/`](src/core/). Each runtime implements the same [hardware abstraction](src/core/hal.h) for terminal I/O, the matrix, the button, the light sensor, and saved progress.

| Runtime | What it connects to |
| --- | --- |
| [RP2350 card](src/card/) | USB serial, the physical matrix and sensors, and a flash save journal |
| [POSIX terminal](src/host/) | A terminal with the matrix above the prompt, keyboard controls, and a local save file |
| [WebAssembly](https://github.com/keetsta/mk1-web) | The browser terminal, a virtual matrix, and virtual controls |

The [technical specification](SPEC.md) covers the serial protocol, save compatibility, and leaderboard contracts.

## Open a terminal

**On a card.** Connect it over USB-C and choose **connect the card** on the mk-1 site in a browser with Web Serial. In a standalone serial terminal, open the card's USB port at 115200 baud with DTR enabled.

**On Linux or macOS.** With a C11 compiler and `make` installed, build and launch the emulator.

```sh
make
make run
```

The emulator saves progress in `~/.mk1-shell`. Set `MK1_STATE` to choose another path.

<details>
<summary>Terminal controls</summary>

| Action | Keys |
| --- | --- |
| Recall a command or clear the screen | <kbd>↑</kbd> / <kbd>↓</kbd> or `cls` |
| Page through the manual | <kbd>Space</kbd> for a page, <kbd>Enter</kbd> for a line, <kbd>q</kbd> to close |
| Press the virtual button or adjust virtual light | <kbd>Space</kbd> or <kbd>+</kbd> / <kbd>-</kbd> while a program runs |
| Stop a program or exit the shell | <kbd>Ctrl</kbd>+<kbd>C</kbd> or <kbd>Ctrl</kbd>+<kbd>D</kbd> on an empty line |

</details>

## Install firmware

On the mk-1 site, open **firmware** to install the latest release or select a local UF2. The installer checks the release SHA-256, rejects images that touch saved progress, and reads the written sectors back before restarting.

When the connected card already runs 0.1.2 or later, the installer matches its chip ID to the chosen bootloader. For older firmware, unplug the other boards, hold BOOTSEL while connecting the target card, and confirm the selected bootloader.

| Release asset | What runs on the card |
| --- | --- |
| `mk1.uf2` | USB shell, standby screen, and matrix screensavers |
| `mk1-demo.uf2` | The same shell with Pong on a button press and a spinning cube on a long press |

<details>
<summary>Install from the RP2350 drive</summary>

For firmware 0.1.2 or later, hold BOOTSEL while plugging in the card and copy the UF2 to the `RP2350` drive.

Use WebUSB for older UF2 files with the RP2350-E10 workaround block in the last sector. Copying such an image to the drive on an RP2350 A2 can erase part of the save journal.

</details>

## Publish a result

`check` verifies a solution and saves your best program for that task. After solving a task, run `top` on a registered card to get a submission link. The leaderboard verifies the card signature and reruns the submitted programs to calculate the score.

Keep the link private because it includes your solutions and can be reused to submit them. To confirm a change to an existing nickname, connect the card to the web terminal.

See how to [run the service and register cards](server/README.md).

## Develop and release

The [CI workflow](.github/workflows/ci.yml) runs the native and leaderboard API tests, builds both UF2 images, and attaches them to the run.

Run `make test` to check the shell core, card UI, and demo. The standard build uses the committed `src/core/content_gen.c`. To test reference solutions, clone the private `mk1-content` repository into `content/`. Run `make content` after editing that content.

<details>
<summary>Build the firmware and render matrix previews</summary>

Install Pico SDK 2.3.1, the Arm GNU Toolchain, CMake, Ninja, and picotool. Set `PICO_SDK_PATH` and, if needed, `PICO_TOOLCHAIN_PATH`. Use `PICOTOOL_DIR` to point to an existing picotool installation.

```sh
make card
make demo
```

The build writes `build/card/mk1.uf2` and `build/card/mk1-demo.uf2`. The preview commands use FFmpeg and write videos to `build/`.

```sh
make preview
make preview-savers
make preview-update
make preview-demo
```

</details>

<details>
<summary>Publish a firmware release</summary>

Push the release commit to `main`, then tag it `vMAJOR.MINOR.PATCH`. To publish 0.1.3, run these commands from that commit.

```sh
git tag -a v0.1.3 -m v0.1.3
git push origin refs/tags/v0.1.3
```

GitHub Actions uses the tag as the firmware version, builds both UF2 files, and publishes them to GitHub Releases. The notes list commit subjects since the previous version tag. The first release lists the full history up to its tag.

The workflow also puts the same `mk1.uf2` on the `firmware-assets` branch under a path containing the version and checksum. When a public repository is configured as the site's release source, the browser reads the version, publication date, and SHA-256 from GitHub Releases and downloads the matching bytes from `raw.githubusercontent.com` for WebUSB installation.

</details>

## License

Original code and documentation are licensed under the [MIT License](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) for materials covered by separate terms.
