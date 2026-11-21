# applixware-wrapper

Run **Applixware 6.0 Home User Edition** (VistaSource, 2006, i386) on a modern
x86_64 Linux desktop, in a rootless podman container with Debian etch i386.

Applixware (née Applix Office / Asterix) was one of the classic Unix office
suites: Words, Spreadsheets, Graphics, Presents, Data, Builder and the ELF
macro language. VistaSource released a free *Home User Edition* of version 6.0
for Linux in 2006. If you still have that tarball
(`applixware6-lnx-homeuser.tar.gz`) and miss it, this wrapper gets it running
again: Words, Spreadsheets, Graphics, licensing, accented input and printing
to PostScript included.

> **This repository contains no Applixware files.** You need your own copy of
> the tarball. The Home User license allows personal/home use only and forbids
> redistributing or modifying the program; this wrapper only provides a
> runtime environment around an unmodified installation.

Tested on openSUSE Leap 16.0 (x86_64, Xorg, podman 5) with Xfce/xfwm4.

## Requirements

- `podman` (rootless is fine), able to run `--arch 386` images
- `python3`, `xauth`, `xset` (X11 session; Wayland only through Xwayland, untested)
- for `--xephyr`: `Xephyr`, `openbox`, `xprop`, `xsetroot`
- for `tools/make-icon.sh`: `Xvfb`, `openbox`, `xdotool`, ImageMagick

## Install

```sh
git clone https://github.com/caiocco/applixware-wrapper.git
cd applixware-wrapper
./install.sh /path/to/applixware6-lnx-homeuser.tar.gz
applixware --install          # accept the Home User license; the key is generated locally
applixware                    # or use the "Applixware" menu entry
./tools/make-icon.sh          # optional: menu icon taken from your own copy
```

Everything goes under your home directory: Applixware in
`~/.local/opt/applixware-6/app`, the launcher in `~/.local/bin/applixware`,
the Openbox theme in `~/.local/share/themes/EMWM-Motif`, the menu entry in
`~/.local/share/applications`. Re-running `install.sh` updates the wrapper and
leaves Applixware and its license alone; files it replaces are kept with a `~`
suffix.

The Home User installer does not copy anything: it shows the license, and on
acceptance generates a FLEXlm key for the machine's hostid into
`app/axlocal/axlicensedat`. The launcher starts the license manager
(`axnlmgrd`/`vistasrc`) inside the container whenever that key needs it.

## Usage

```sh
applixware                     # main window
applixware -wp | -ss | -gr     # straight into Words / Spreadsheets / Graphics
applixware letter.aw           # open a document (paths under mounted dirs)
applixware --xephyr [...]      # same, inside a Xephyr window (see below)
applixware --shell             # shell inside the container
```

Running `applixware` again while it is open adds windows to the same session,
as long as it is the same display (`--xephyr` reuses an open Xephyr session).
Only one session can run at a time (the license is bound to the container);
starting one on another display is refused.

Settings go in the environment or in `~/.config/applixware-wrapper.conf`
(a shell fragment):

| Variable | Default | Meaning |
|---|---|---|
| `AX_MOUNTS` | *(none)* | extra host directories to expose, `:`-separated (`$HOME` is always mounted) |
| `AX_BASE` | `~/.local/opt/applixware-6` | where Applixware and the Openbox config live |
| `AX_XEPHYR_SIZE` | `1920x900` | Xephyr screen size |
| `AX_XEPHYR_DPI` | `96` | Xephyr DPI |
| `AX_XEPHYR_WM` | Openbox + EMWM-Motif | window manager command for `--xephyr`, any program in `PATH` (classic WMs do not work, see caveats) |
| `AX_LC_CTYPE` | `en_US` | any ISO-8859-1 locale in the image (`pt_BR`, `de_DE`, `fr_FR`, `es_ES`, `it_IT`) |
| `AXLANG` | `1` | Applixware UI language: 1 English, 2 German, 3 French |

Logs of the last session: `$XDG_RUNTIME_DIR/applixware.log` (Applixware and
license manager), `applixware-xephyr.log`, `applixware-wm.log`.

## What had to be worked around

Each item cost some debugging; they are documented in the scripts too.

- **GCC 2.95 C++ runtime.** The binaries need `libstdc++-libc6.2-2.so.3`;
  Debian etch i386 (`docker.io/debian/eol:etch`) still has it.
- **FLEXlm hostid = MAC address.** Rootless podman gives a random MAC per run,
  which would invalidate the key. The container runs on podman's bridge
  network with a fixed MAC and hostname (`AX_MAC`, `AX_HOST`).
- **64-bit inode numbers.** The 2006 binaries use 32-bit `stat()`/`readdir()`,
  which fail with `EOVERFLOW` on XFS/ext4 with large inode numbers: the font
  path "breaks", `axmain` dies, files cannot be opened. `container/ino32shim.c`
  is an `LD_PRELOAD` shim that calls the 64-bit variants and folds the inode.
  (On tmpfs everything works, which hides the bug.)
- **8-bit text.** Without an ISO-8859-1 `LC_CTYPE` Applixware strips the 8th
  bit of what you type (é→i, ç→g, ©→`)`).
- **Dead keys.** Applixware composes dead keys itself from
  `axdata/deadkeys.dat`. `tools/deadkeys-us-intl.sh` (run by `install.sh`)
  adds `'`+`c` → ç and dead key + space → the accent itself, for US-International.
- **Fonts.** Applixware adds its bitmap font directory to the X server font
  path, so the installation is mounted at the same absolute path in the
  container. Its bundled font server (gfs) would listen inside the container;
  instead the launcher adds the TrueType directory to the host X server with
  `xset +fp`, and passes `-fp` so the UI fonts are found.
- **Focus loop.** At startup, if the new main window does not get the input
  focus while the splash screen holds a keyboard grab, `axmain` loops on
  FocusIn/FocusOut events until it exceeds `axMemoryLimit` ("High Memory Usage"
  popups, then exit). Modern window managers (xfwm4, Openbox, ...) are fine;
  bare X servers and classic ones (**mwm, EMWM, fvwm2, twm**) trigger it.
  Applixware's own *Allow Window Manager to choose Focus* preference
  (`axDisableFocusManage`, meant for click-to-type WMs) does not help: the
  splash screen grabs the keyboard before it matters.

## Caveats

- **Graphics trails.** On Xorg with glamor acceleration (seen with modesetting
  + radeonsi), the XOR rubber bands of Applix Graphics leave trails when moving
  or creating shapes, compositor or not. `applixware --xephyr` runs Applixware
  inside Xephyr (software rendering), where they are clean. The menu entry has
  "Applixware (Xephyr)" and "Applix Graphics (Xephyr)" actions for that.
  Xephyr has its own clipboard, not shared with the desktop.
- **Classic window managers** cannot be used inside Xephyr (focus loop above).
  The default is Openbox with the bundled **EMWM-Motif** theme, which imitates
  EMWM/Motif decorations: menu button (click = window menu, double-click =
  close), centred title, maximize button and EMWM default colours. Minimize
  uses an OS/2-style *hide* glyph instead of the Motif one, since Openbox
  does not iconify windows onto the root window.
- **Printing:** print to a PostScript file and convert it (`ps2pdf`). There is
  no CUPS inside the container.
- Do not run Applixware's own `install` again elsewhere: the key is bound to
  the fixed hostname/MAC of the container.

## Layout

```
bin/applixware                 launcher
container/Containerfile        i386 Debian etch image
container/ino32shim.c          LD_PRELOAD shim for 64-bit inodes
openbox/rc.xml                 Openbox config for --xephyr
themes/EMWM-Motif/openbox-3/   Openbox theme imitating EMWM
desktop/applixware.desktop.in  menu entry template
tools/deadkeys-us-intl.sh      US-International dead keys for Applixware
tools/make-icon.sh             menu icon from your own installation
install.sh                     per-user installer
```

## Resumo em português

Wrapper para rodar o **Applixware 6.0 Home User Edition** (2006, i386) num
Linux x86_64 atual, dentro de um contêiner podman com Debian etch i386. **O
repositório não contém nada do Applixware**: é preciso ter o seu próprio
tarball (`applixware6-lnx-homeuser.tar.gz`), e a licença Home User permite só
uso pessoal, sem redistribuição.

```sh
./install.sh /caminho/applixware6-lnx-homeuser.tar.gz
applixware --install     # aceite a licença Home User; a chave é gerada localmente
applixware               # ou pelo menu
```

Contornos incluídos: biblioteca C++ do GCC 2.95, hostid do FLEXlm (MAC fixo no
contêiner), shim para inodes de 64 bits (XFS/ext4), locale Latin-1 para os
acentos, cedilha com `'`+`c` no teclado US-Internacional, fontes no servidor X
do host. No Xorg com aceleração glamor, o Graphics deixa rastros ao mover
formas; use `applixware --xephyr` (ou a ação "Applix Graphics no Xephyr" do
menu), que roda dentro de um Xephyr com Openbox e um tema imitando o EMWM.
WMs clássicos (mwm, EMWM, fvwm2, twm) disparam um laço de foco na abertura e
não servem dentro do Xephyr.

## License

The wrapper (scripts, Containerfile, shim, Openbox config and theme) is
GPL-3.0-or-later; see `LICENSE`. Applixware is © VistaSource, Inc. and is not
part of this project.
