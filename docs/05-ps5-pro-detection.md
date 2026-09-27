# 05 — PS5 Pro detection

The plan was to pick the AI network by console: the small one on a base PS5,
the big ones on a PS5 Pro (code name **Trinity**). Two libkernel functions
answer "is this a Pro":

| Function | NID | Meaning |
|---|---|---|
| `sceKernelHasTrinityMode` | `yu17wG8L5FI` | the title is running in Pro mode |
| `sceKernelIsAuthenticTrinity` | `X0HkB92+NRE` | the hardware is a Pro |

## Resolving them safely

- **Why not import them directly.** Neither function is exported by the
  homebrew SDK's libkernel stubs. On this platform a missing import doesn't
  fail cleanly: the loader binds it to address 0, and the app module crashes
  at load with no log.
- **What EVO does instead.** It resolves them at runtime with
  `sceKernelDlsym`, which *is* in the stubs:
  1. Try handle `0x2001` (where the loader puts libkernel), then `0x2`.
  2. Otherwise ask the loader for `libkernel_sys.sprx` / `libkernel.sprx`.
  3. For each handle, try the symbol name, then the NID string.
- **If none resolve,** the answer is "unknown", which every caller treats as a
  base PS5.

Code: [`examples/runtime/evo_hw.c`](../examples/runtime/evo_hw.c).

## What actually happened on a PS5 Pro (FW 12.70)

```
hw: dlsym sceKernelHasTrinityMode handle=0x2001 rc_name=0x80020003 rc_nid=0x80020003
hw: load libkernel_sys.sprx -> 0x80020002
hw: dlsym sceKernelIsAuthenticTrinity handle=0x2001 rc_name=0x80020003 rc_nid=0x80020003
hw: dlsym control sceKernelUsleep -> not found (0)
hw: ps5 pro=? trinity_mode=? authentic=?
```

- **`0x80020003` is ESRCH, and it means nothing about Trinity.** A control
  lookup of `sceKernelUsleep`, a libkernel function EVO calls every frame,
  fails with the same code. **`sceKernelDlsym` resolves nothing from a
  fake-signed app module on this firmware.** That holds by name and by NID,
  on handles `0x2001` and `0x2`, and on the handle `sceKernelLoadStartModule`
  returns for `libkernel.sprx`.
- **`0x80020002` is ENOENT.** `libkernel_sys.sprx` can't be loaded from an app
  module.

An earlier version of this page blamed libkernel for hiding the Trinity
functions. The control lookup showed that was wrong: the lookup mechanism
itself is unusable.

## Which query matters, and how to ask it

Sony's own PSSR library (`libScePsml.sprx`, in `/system/common/lib`) refuses
to run unless `sceKernelIsTrinityMode()` is true:

```
[PSML] MFSR isn't supported in sceKernelIsTrinityMode() == 0
```

- **The NID.** `sceKernelIsTrinityMode` is NID `tU5e3f9gSiU`, computed with
  the standard `sha1(name + suffix)` scheme and checked against known pairs.
- **It's a plain libkernel export.** `libScePsml` imports it as `#G#H`, its
  libkernel module/library record, the same slot as `sceKernelUsleep`.
- **But an app can't import it.** A one-line link stub (SONAME
  `libkernel.sprx`) produced an import record identical to Sony's own library,
  `tU5e3f9gSiU#G#H`. The loader **rejected EVO at launch**: no log was
  written, and the app crashed before `main()`. libkernel exports this
  function to system modules, not to fake-signed apps.

The title also has to *be* in Pro mode. Every PS5 Pro-enhanced game on the test
console carries two things in its `param.json` that non-enhanced titles lack:

| Title | `psml` | `attribute3` |
|---|---|---|
| Avatar: Frontiers of Pandora | `{"mfsrVersion": "11.00"}` | `0x08400040` |
| The Last of Us Part I | `{"mfsrVersion": "09.60"}` | `0x004400D4` |
| Assassin's Creed Shadows | `{"mfsrVersion": "11.00"}` | `0x08440054` |
| Alan Wake Remastered (not enhanced) | — | `0` |
| EVO Player | — | `0x00080040` |

Bit **`0x00400000`** of `attribute3` is the only one all three Pro titles share
and EVO lacks. It is the likely "PS5 Pro enhanced" flag. With it and the `psml`
block added, EVO still launches normally. But with `dlsym` dead and the direct
import refused, EVO has no way to observe whether it's in Trinity mode, so
the flags were not kept.

## The fix: let the user choose

Detection only matters for picking a default. Every network is plain shader
code and runs on any PS5, so EVO's **Settings → AI NETWORK** offers:

| Option | Runs |
|---|---|
| Auto | Large on a *detected* Pro, Standard otherwise |
| Standard | Anime4K S |
| Large | Anime4K M |
| Maximum (Pro) | Anime4K UL |

- **A manual choice overrides detection,** so a Pro the probe can't identify
  still gets the big networks.
- **A base PS5 that picks Maximum** is protected by the GPU-budget fallback
  (Maximum → Large → Standard → Sharp).
- **The CONSOLE row** in System & Diagnostics shows **NOT DETECTED** rather
  than wrongly claiming "PS5".

## Open questions

- **A readable source for the model.** A sysctl, a VideoOut or system-service
  query, anything read-only that works from an app module, would make Auto
  meaningful. The three Trinity queries are out of reach: `dlsym` is dead and a
  direct import is refused.
- **Sony's single-image upscaler.** The `scePsmlBcSisr*` API (Init,
  BuildPacket, GetTexture, Term; model `BCSISR_v070.psp`, a U-Net) is exported
  only by `/system/priv/lib/libScePsmlBcSisr.sprx`, which fake-signed apps
  can't import. It links against the system shell's graphics library, not the
  game-side `libSceAgc`. It is Sony's system-level upscaler, not an app API.
- **PSSR itself** (`scePsmlMfsr*` in the importable `libScePsml`) is temporal
  and needs a game's motion vectors, depth and jitter. Decoded video has none
  of these.
