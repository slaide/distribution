# Game performance — SM8750 (KONKR Pocket FIT Elite)

Steam/FEX games: observed status + fps per config, with the stack versions at
test time. Add a dated line on retest, don't overwrite.

## Horizon Zero Dawn Remastered (2561580)

2026-07-02 — **runs, not playable (~12 fps)**
Stack: kernel 7.0.11, mesa 26.1.2, proton-cachyos 20260602 (wine-11.0, FEX-2604-164), game build 17237254

Required to run: `FEX_X87REDUCEDPRECISION=0` (= "PlayStation" preset; Sony SDK
anti-tamper, FEX#4556) and `vm.max_map_count=2147483642` (now system default).

| Config | Result |
|---|---|
| main menu | 112 fps |
| 720p FSR3 ultra-perf, very low | 12 fps |
| 720p native, very low | 12 fps |
| 720p native, medium | 12 fps |
| 1080p native, medium | 10 fps |
| high settings | crash (out of memory) |
| `VKD3D_CONFIG=single_queue` | intermittent vkQueueSubmit2 assert; 12 fps otherwise |

No effect: present mode, dstorage.dll removal, ntsync toggle, `no_upload_hvv`,
FEX presets beyond the required flag. Game speed correct. fps is invariant to
GPU pixel load and CPU headroom — retest on Turnip/vkd3d/proton bumps.

## Lies of P (1627720)

2026-07-02 — **playable (35–42 fps)**
Stack: kernel 7.0.11, mesa 26.1.2, proton-cachyos 20260602 (wine-11.0, FEX-2604-164); FEX preset Intermediate.

| Config | Result |
|---|---|
| 720p FSR3 ultra-perf, lowest | 70 fps (peak) |
| 1080p FSR3 highest-perf, low | 42 fps |
| 1080p FSR3 highest-perf, medium (visibility low) | 42 fps |
| 1080p FSR3 highest-perf, high (visibility low) | 35 fps |

Stutter noticeably reduced by the vm.max_map_count bump (same-day system change).

## The First Berserker: Khazan (2680010)

2026-07-09 — **requires `-dx12`; runs, not playable (~9 fps, 1080p low)**
Stack: kernel 7.0.11, mesa 26.1.2, proton-cachyos 20260602 (wine-11.0, FEX-2604-164), FEX preset Intermediate, game build 22579715

| Config | Result |
|---|---|
| default launch (no `-dx12`) | crash during shader compilation, before main menu |
| `-dx12`, 1080p low | in-game, ~9 fps |

`-dx12` is required to launch: without it the game crashes while compiling
shaders before the main menu. With it you reach gameplay, but only ~9 fps at
1080p low.
