# Third-party notices

This project builds on third-party work. Their licenses are reproduced below.

---

## OpenOMF

Source: <https://github.com/omf2097/openomf> — license: **MIT**.

A copy of OpenOMF is vendored, unmodified, under [`openomf-master/`](openomf-master/)
(see `openomf-master/LICENSE`). This port **derives heavily** from it:

- The host-side asset tools (`port_3do/host_tools/`) link a subset of OpenOMF's
  `.c` files (formats/, utils/, video/) to read OMF:2097 data.
- The firmware links / ports OpenOMF logic (e.g. `intersect.c`, and ported
  har/player/move systems).

OpenOMF MIT license text:

```
Copyright (C) 2097 Tuomas Virtanen, Andrew Thompson, Hunter and others

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Trapexit 3do-devkit

Source: <https://github.com/trapexit/3do-devkit>.

Required to build (toolchain + `System/` boot files). **Not redistributed here.**
The 3DO system files it provides are © The 3DO Company.

---

## One Must Fall: 2097

© Diversions Entertainment / Epic MegaGames. Freeware since 1999, but **not
redistributable**. No OMF:2097 data is included in this repository; you supply
your own copy. This is an unaffiliated fan project.
