# typst-automata

A small drag-to-arrange viewer for finite automata. It reads a Graphviz DOT
file, uses Graphviz's layout for the starting positions, lets you drag the
states and edge labels where you want them, then exports the diagram as
[fletcher](https://typst.app/universe/package/fletcher) code for Typst.

## Requirements

- A C compiler, `make` and `pkg-config`
- [raylib](https://www.raylib.com/) 5.5 (the window and drawing)
- [Graphviz](https://graphviz.org/) (the program runs `dot` at startup to lay
  out the automaton)
- [Typst](https://typst.app/) to compile the exported diagrams (fletcher is
  downloaded by Typst automatically)

## Building on Ubuntu

These steps were tested on fresh Ubuntu 22.04 and 24.04 installs.

### 1. Install the compiler and Graphviz

```sh
sudo apt install build-essential pkg-config graphviz
```

### 2. Install raylib (skip this if you already have it)

Ubuntu doesn't package raylib, so build it from source. First install the
libraries raylib needs:

```sh
sudo apt install git cmake libasound2-dev libx11-dev libxrandr-dev libxi-dev \
    libgl1-mesa-dev libglu1-mesa-dev libxcursor-dev libxinerama-dev \
    libwayland-dev libxkbcommon-dev
```

Then build and install raylib 5.5 as a shared library under `/usr/local`:

```sh
git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git
cd raylib
cmake -B build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig
cd ..
```

`sudo ldconfig` refreshes the system's library cache so the program can
find `libraylib.so` when it runs. To check the install worked:

```sh
pkg-config --modversion raylib    # should print 5.5.0
```

If you want to remove raylib later, run
`sudo xargs rm < build/install_manifest.txt` from the `raylib` directory.

### 3. Build the viewer

From this repository:

```sh
make
```

This produces `bin/dfa`. Try it with `make run`, which opens
`examples/ends-abb.dot`.

## Building on Fedora

Fedora packages raylib, so one command installs everything:

```sh
sudo dnf install gcc make pkgconf raylib-devel graphviz
make
```

## Usage

```sh
./bin/dfa [-o out.typ] [--export] [--screenshot out.png] automaton.dot
```

- `-o out.typ`: where to write the fletcher code (default: the DOT file's
  name with `.typ`, e.g. `automaton.typ`)
- `--export`: write the fletcher code for Graphviz's layout and exit without
  opening a window
- `--screenshot out.png`: render one frame to a PNG and exit

### Controls

| Key / mouse | Action                                              |
| ----------- | --------------------------------------------------- |
| Left-drag   | Move a state or an edge label                       |
| Right-click | Reset an edge label to fletcher's default placement |
| `G`         | Toggle grid snapping                                |
| `E`         | Export fletcher code                                |
| `C`, `Esc`  | Enter / leave colour mode                           |
| `Esc`       | Quit (outside colour mode)                          |

In colour mode, press `1`–`7` or click a swatch to pick a fill colour.
Left-click a state to fill it (click again to clear), or right-click it to
clear its fill.

### Writing the DOT file

See `examples/` for complete files. The conventions are:

- `shape=doublecircle` marks an accepting state.
- A node is invisible if it has `shape=point`, `style=invis`, or
  `shape=none` / `shape=plaintext` with no label.
- An invisible node whose edges are all unlabelled and outgoing marks their
  targets as initial states, e.g. `start [shape=point]; start -> q0;`.
- Any other invisible node is a loose arrow end: it's shown as a dashed
  handle and exported as a coordinate rather than a node.
- Several edges between the same pair of states with the same style are
  merged into one arrow, with their labels joined by `, `.
- `style=dashed` on an edge draws a dashed arrow.
- Edge labels are typeset as math. Labels already written as `$...$` are
  passed through unchanged.

### Using the output in Typst

The exported `.typ` file is a self-contained diagram. Compile it on its own
with `typst compile automaton.typ`, or pull it into a document with
`#include "automaton.typ"`. Exporting again overwrites the file.
