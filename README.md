# dwlb

![screenshot 1](/screenshot1.png "screenshot 1")
![screenshot 2](/screenshot2.png "screenshot 2")

dwlb is a simple bar for [dwl](https://github.com/djpohly/dwl). It is a modified version of [dtao](https://github.com/djpohly/dtao).

## Installation
```bash
git clone https://github.com/kolunmi/dwlb
cd dwlb
make install
```

## Usage
Pass `dwlb` as an argument to dwl's `-s` flag. This will populate each connected output with a bar. For example:
```bash
dwl -s 'dwlb -font "monospace:size=16"'
```

## Commands
Command options send instructions to existing instances of dwlb. All commands take at least one argument to specify a bar on which to operate. This may be zxdg_output_v1 name, "all" to affect all outputs, "selected" for the current output, or "first" for the first output in the internal list.

### Status Text
The `-status` command is used to write status text. The text may contain in-line color commands in the following format: `^fg/bg(HEXCOLOR)`. For example, `^fg(ff0000)` would set the foreground red. Colors can be reset by omitting the hex value. `^^` represents a single `^` character. Color command functionality can be disabled with `-no-status-commands`.

## Other Options
Run `dwlb -h` for a full list of options.
