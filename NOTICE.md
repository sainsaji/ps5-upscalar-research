# Notices

## Code

The code in `examples/` and `tools/` is part of, or derived from,
[EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5) and is licensed under
the **GNU General Public License v3.0 or later**. See [LICENSE](LICENSE).

## Anime4K

`third_party/anime4k/*.glsl` are vendored unchanged from
<https://github.com/bloc97/Anime4K>. The generated shaders in
`examples/pipes/upscale_a4k_*.pipe` contain its network weights.

```
MIT License

Copyright (c) 2019-2021 bloc97
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

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

The full licence file is `third_party/anime4k/LICENSE`.

## AMD FidelityFX Super Resolution 1.0

The EASU and RCAS shaders (`EASU_FS` / `RCAS_FS` in
`examples/generator/gen_upscale_pipes.py`, and the `upscale_easu` /
`upscale_rcas` pipes) are a GLSL port of the float path of AMD's `ffx_fsr1.h`.

```
Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Images

Every image under `comparisons/` is a frame of one of these films, captured
from a PS5 after upscaling:

- *Big Buck Bunny* — © 2008 Blender Foundation | <https://peach.blender.org>
- *Tears of Steel* — © 2012 Blender Foundation | <https://mango.blender.org>

Both films are licensed under the
[Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
licence. The captures and every crop, strip, zoom and grid made from them are
derivative works (upscaled, cropped, labelled), released under the same
CC BY 3.0 licence.

The 720p and 540p *Big Buck Bunny* sources were re-encoded from the 1080p
release for testing; see `docs/07-reproducing.md`.
