# Speeduino base tunes

**These files are NOT under the GPL.** They are third-party works licensed under the
Creative Commons Attribution-NonCommercial 4.0 International license (CC BY-NC 4.0),
full text in `LICENSE` (also https://creativecommons.org/licenses/by-nc/4.0/).

| | |
|---|---|
| Source | https://github.com/speeduino/Tunes |
| Commit | 99b2a62e402144fa38c911f937b2b6571c8a720a (2025-03-03) |
| License | CC BY-NC 4.0 (`LICENSE`, copied unmodified from the source repository) |
| Attribution | Speeduino project and contributors; the author of each tune is the `provider` in `index.json` |
| Changes | None: the `.msq` files, `index.json` and `LICENSE` are byte-identical copies |

## Tunes

| Make | Tune | Board | Provider | File |
|------|------|-------|----------|------|
| Generic | Speeduino Generic base tune | v0.4 | Speeduino | `Generic/Speeduino base tune.msq` |
| Mazda | Miata / MX5 89-95 PNP | NA6 PNP | Speeduino | `Mazda/NA6 PNP base tune.msq` |
| Mazda | Miata / MX5 96-97 PNP | NA8 PNP | Speeduino | `Mazda/NA8 PNP base tune.msq` |
| Mazda | Miata / MX5 96-98 PNP (SpeedyEFI) | UA4C | SpeedyEFI | `Mazda/Miata_PNP96-98_SpeedyEFI.msq` |
| Mazda | Miata / MX5 99-00 PNP (SpeedyEFI) | UA4C | SpeedyEFI | `Mazda/Miata_PNP99-00_SpeedyEFI.msq` |
| Mazda | Miata / MX5 01-05 PNP | NB2 PNP | Speeduino | `Mazda/NB8B PNP base tune.msq` |
| BMW | BMW M50NV PNP | BMW PNP | Pazi88 | `BMW/M50NV_PnP.msq` |
| BMW | BMW M50TU PNP | BMW PNP | Pazi88 | `BMW/M50TU_PnP.msq` |
| BMW | BMW M52 PNP | DIY-EFI CORE4 | Pazi88 | `BMW/M52_PnP.msq` |
| BMW | BMW M43 PNP | BMW PNP | Pazi88 | `BMW/M43_PnP.msq` |
| BMW | BMW M60 PNP | BMW PNP | Pazi88 | `BMW/M60_PnP.msq` |
| BMW | BMW M52TU PNP | DIY-EFI CORE4 | Pazi88 | `BMW/M52TU_PnP.msq` |
| Mitsubishi | 4G93 | v0.4 | Steven Chua | `Mitsubishi/4g93_stock.msq` |
| Audi | A4 B5 1.8t | UA4C | N/A (not named upstream) | `Audi/Audi_A4_B5_1.8t.msq` |

These are the 14 tunes listed in the upstream `index.json`. The upstream repository also holds
`Generic/Speeduino base tune with wmi.msq`, which `index.json` does not list; it is not included.
`index.json` is kept as published (it uses the id "12" twice; the program does not use the ids).

## How clibretune uses them

`tools/gen_basetunes.py` embeds the files unmodified (packed with a lossless LZ77 coder, about 75 KB,
unpacked byte for byte at run time) into `src/tune/basetune_data.c`, which
carries the same license notice. The program shows them in File > New Project from Base Tune and
File > Load Base Tune, with the tune's author and this license. When a base tune is loaded into a
project with a different ECU definition, the values are converted to that definition (an
adaptation; the original files stay unchanged here), and the saved tune records which base tune it
came from.

## Non-commercial use

CC BY-NC 4.0 does not allow commercial use. clibretune is free software and is never sold; any
binary or bundle that contains these tunes (every build of clibretune embeds them) must not be sold
or otherwise used primarily for commercial advantage. Anyone who wants to distribute clibretune
commercially must first regenerate `src/tune/basetune_data.c` without the tunes
(`python3 tools/gen_basetunes.py --empty`) and leave this folder out.
