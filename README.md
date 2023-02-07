# dwlb

![screenshot](/screenshot.png "screenshot")

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

## Status Text
The `-status` option sends status text to existing instances of dwlb. This takes two arguments: a zxdg_output_v1 name (alternatively "all" to affect all outputs or "selected" for the current output) and the text itself.

The text may contain in-line color commands in the following format: `^fg/bg(HEXCOLOR)`. For example, `^fg(ff0000)` would set the foreground red. Colors can be reset by omitting the hex value. `^^` represents a single `^` character.

## Other Options
Run `dwlb -h` for a full list of options.
