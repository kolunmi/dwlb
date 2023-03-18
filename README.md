<div align="center">
<h1>dwlb</h1>

A fast, feature-complete bar for [dwl](https://github.com/djpohly/dwl).

![screenshot 1](/screenshot1.png "screenshot 1")
![screenshot 2](/screenshot2.png "screenshot 2")
</div>

## Dependencies
* libwayland-client
* libwayland-cursor
* pixman
* fcft

## Installation
```bash
git clone https://github.com/kolunmi/dwlb
cd dwlb
make
make install
```

## Usage
Pass `dwlb` as an argument to dwl's `-s` flag. This will populate each connected output with a bar. For example:
```bash
dwl -s 'dwlb -font "monospace:size=16"'
```

## Ipc
If dwl is [patched](https://lists.sr.ht/~raphi/public-inbox/patches/39166) appropriately, dwlb is capable of communicating directly with dwl. When ipc is enabled with `-ipc`, dwlb does not read from stdin, and clicking tags functions as you would expect. Ipc can be disabled with `-no-ipc`.

## Commands
Command options send instructions to existing instances of dwlb. All commands take at least one argument to specify a bar on which to operate. This may be zxdg_output_v1 name, "all" to affect all outputs, or "selected" for the current output.

### Status Text
The `-status` and `-title` commands are used to write status text. The text may contain in-line commands in the following format: `^cmd(argument)`.

| In-Line Command     | Description                                                                 |
|---------------------|-----------------------------------------------------------------------------|
| `^fg(HEXCOLOR)`     | Sets foreground color to `HEXCOLOR`.                                        |
| `^bg(HEXCOLOR)`     | Sets background color to `HEXCOLOR`.                                        |
| `^lm(SHELLCOMMAND)` | Begins or terminates left mouse button region with action `SHELLCOMMAND`.   |
| `^mm(SHELLCOMMAND)` | Begins or terminates middle mouse button region with action `SHELLCOMMAND`. |
| `^rm(SHELLCOMMAND)` | Begins or terminates right mouse button region with action `SHELLCOMMAND`.  |

In this example, clicking the text highlighted in red will spawn the [foot](https://codeberg.org/dnkl/foot) terminal.
```bash
dwlb -status all 'text ^bg(ff0000)^lm(foot)text^bg()^lm() text'
```

A color command with no argument reverts to the default value. `^^` represents a single `^` character. Status commands can be disabled with `-no-status-commands`.

## Other Options
Run `dwlb -h` for a full list of options.

## Someblocks
To use someblocks, or any program that outputs to stdout, with dwlb, use this one-liner:
```bash
someblocks -p | while read -r line; do dwlb -status all "$line"; done
```

## Acknowledgements
* [dtao](https://github.com/djpohly/dtao)
* [somebar](https://sr.ht/~raphi/somebar/)
