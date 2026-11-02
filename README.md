# Datamosh for OBS

A datamosh video filter for OBS Studio on Windows. Instead of updating to the next frame, the picture gets dragged along by whatever is moving, like a video file with its keyframes torn out. Everything runs on the GPU, live, with sliders.

![raw / smooth mosh / block mosh / amount 0](docs/preview.png)
*Clean source · smooth mosh · block mosh · Amount 0*

## Requirements

- OBS Studio **31.1 or newer** (made for 32.1.0), 64-bit Windows 10 or 11
- Any GPU that runs OBS

## Install

1. Close OBS.
2. Download **`datamosh-filter.zip`** from the [latest release](../../releases/latest).
3. Unzip it into `C:\Program Files\obs-studio\`. It contains just one file, which lands at:
   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\datamosh-filter.dll
   ```
   Windows will ask for admin permission to copy into Program Files; that's normal.
4. Start OBS.

## Use it

1. Right-click your camera, video or capture source and choose **Filters**.
2. Under **Effect Filters**, click **+** → **Datamosh**, give it a name, click OK.
3. Move. Wave a hand, turn your head, pan the camera, and the picture smears along with it.

To start a fresh mosh, click **Clean frame (reset)**, then move. Turn the effect off with the eye icon next to the filter.

### Hotkey

In **Settings → Hotkeys**, type `datamosh` in the filter box to find **Datamosh: clean frame**. Bind it to snap back to the real picture mid-stream, then move again for a new smear.

## Sliders

| Slider | What it does | Default |
|---|---|---|
| **Amount** | Mix between the clean source (0) and the full mosh (1) | 0.9 |
| **Refresh (how fast it heals)** | How fast the real picture leaks back in. 0 = it never heals | 0.02 |
| **Flow scale** | Multiplies the motion. Above 1 smears harder; 0 freezes the smear in place | 1 |
| **Block px (0 = smooth)** | 0 melts smoothly. 4–32 moves the picture in hard square macroblocks, like a broken file | 0 |
| **Steady flow** | Blurs the motion so it swims instead of jittering | 0.3 |
| **Auto reset every N s (0 = off)** | Drops in a clean frame on a timer | 0 |
| **Motion search range** | How far it looks for movement each frame (×4 pixels). Lower is lighter on the GPU | 6 |

## Recipes

- **Classic melt:** Refresh 0, Block px 0, Steady flow 0.5. Hit reset, then move slowly.
- **Broken file:** Block px 16, Refresh 0.01, Steady flow 0.
- **Ghost trails:** Amount 0.6, Refresh 0.1. A light smear that cleans itself up.
- **Hands-free:** Auto reset 4 s, Refresh 0. A fresh mosh every few seconds.

## Good to know

- Areas of flat, single colour don't move. There's nothing in them to track, so texture and edges smear best.
- Motion is measured on the source itself, so a webcam with you moving works just as well as a panning video.
- With a huge source (4K) and a weak GPU, lower **Motion search range** first.

## Troubleshooting

**Datamosh isn't in the + menu.** The DLL isn't in the right folder, or OBS was open while you copied it. Check the path in step 3 of Install, then restart OBS.

**The filter is there but the picture is unchanged or black.** Open **Help → Log Files → View Current Log** and search for `datamosh`. If there's an `effect failed to compile` line, [open an issue](../../issues) and paste that line.

## Uninstall

Close OBS and delete `C:\Program Files\obs-studio\obs-plugins\64bit\datamosh-filter.dll`.

## Build from source

Cross-compiles from Linux with zig, so you don't need Visual Studio:

```bash
ZIG=zig OBS_SRC=path/to/obs-studio ./build.sh
```

Needs [zig](https://ziglang.org) 0.14 and an obs-studio source tree (only the `libobs/` headers are used). Output: `dist/datamosh-filter.dll`. `./build.sh linux` with `OBS_LIB=` also builds a `.so` for testing in Linux OBS.

The shader is in [`src/mosh.effect`](src/mosh.effect): block-matched motion between frames drags a buffer of old frames along, and the real picture leaks back in at the Refresh rate.

## License

GPL-2.0-or-later, like OBS itself.
