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
hw: dlsym sceKernelHasTrinityMode handle=0x2 rc_name=0x80020003 rc_nid=0x80020003
hw: load libkernel_sys.sprx -> 0x80020002
hw: dlsym sceKernelHasTrinityMode handle=0x2001 rc_name=0x80020003 rc_nid=0x80020003
hw: dlsym sceKernelIsAuthenticTrinity handle=0x2001 rc_name=0x80020003 rc_nid=0x80020003
...
hw: ps5 pro=? trinity_mode=? authentic=?
```

- **`0x80020003` is ESRCH.** Handle `0x2001` is libkernel: loading
  `libkernel.sprx` by name hands back that same handle. So libkernel is found,
  but it doesn't export these symbols to a fake-signed app. The failure is the
  same by name and by NID.
- **`0x80020002` is ENOENT.** `libkernel_sys.sprx`, which likely does export
  them, can't be loaded from an app module.

So **from a homebrew app, these queries are unreachable on this firmware**, and
the console is reported as "model unknown".

Worth checking before relying on a stub: `llvm-nm -D` on the SDK's
`libkernel*.so` shows `sceKernelDlsym` and `sceKernelLoadStartModule` present,
and neither Trinity function.

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

- **Another source for the model.** Candidates: a model-name call, a sysctl
  model string, or the CFI-7xxx model number. Anything that works from an app
  module would make Auto meaningful.
- **Whether a Pro-enhanced title flag changes the answer.** `HasTrinityMode`
  is about the *title* running in Pro mode.
- **PSSR / PSML.** Sony's own ML upscaler on the Pro lives behind a system
  library. Whether an app module can reach it is untested. It would need the
  module to be in `/system/common/lib`; `/system/priv/lib` is off-limits to
  fake-signed apps.
