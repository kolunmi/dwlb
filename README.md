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
```bash
dwl -s dwlb
```

## Status Text
The `-status` option sends status text to existing instances of dwlb. This takes two arguments: a zxdg_output_v1 name (alternatively "all" to affect all outputs or "selected" for the current output) and the text itself.

## Other Options
Run `dwlb -h` for a full list of options.
