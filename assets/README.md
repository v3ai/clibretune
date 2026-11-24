# Assets

Everything here is embedded into the binary at build time by generator scripts;
the build itself does not read these files.

| Path | What | License |
|------|------|---------|
| `fonts/IBMPlex*.ttf` | IBM Plex Sans (Regular, Medium, SemiBold) and IBM Plex Mono (Regular, Medium), unmodified from the official IBM/plex releases (`@ibm/plex-sans@1.1.0`, `@ibm/plex-mono@2.5.0`) | SIL OFL 1.1, `fonts/OFL.txt` |
| `icons/*.svg` | The Lucide icons used by the UI, from lucide-static 0.562.0 | ISC, `icons/LICENSE` |
| `definitions/` | Manifests (`*/sources.json`: URL, tag, commit, sha256) of the 167 bundled ECU definitions: Speeduino releases 201712..202501.7, rusEFI release_20260812 (all boards), release_20260220_2 and release_20251018, FOME release_260501_02 (all boards), miata-ecus; the INI files themselves are fetched into `build/definitions/`, details in its `README.md` | Speeduino GPL-2.0-or-later; rusEFI, FOME, miata-ecus **GPL-3.0**: a binary containing them is GPL-3.0 |
| `basetunes/speeduino/` | The 14 Speeduino base tunes of https://github.com/speeduino/Tunes, unmodified (source commit, authors and details in its `README.md`) | **CC BY-NC 4.0, not GPL**, `basetunes/speeduino/LICENSE` |

Regenerate the C data:

    python3 tools/gen_embed.py                       # src/gui/fonts_data.c (subset, <=24000-byte arrays)
    python3 tools/gen_icons.py                       # src/gui/icons.h + icons_data.c
    python3 tools/gen_icons.py --fetch LUCIDE_DIR    # first re-copy SVGs from an extracted lucide-static
    python3 tools/gen_basetunes.py                   # src/tune/basetune_data.c (CC BY-NC 4.0 data)
    python3 tools/gen_basetunes.py --empty           # same without any tune (a build that may be sold)
    python3 tools/fetch_definitions.py               # download the ECU definitions to build/definitions/
    python3 tools/gen_definitions.py                 # src/project/inidb_data.c (packed INI library)
    python3 tools/gen_definitions.py --only speeduino  # same without the GPL-3.0 files

The embedded fonts are subset (Latin-1, Latin Extended-A, Greek, common punctuation and
symbols) by `tools/gen_embed.py`; glyph ids and kerning are unchanged. The OFL requires the
license text to travel with the fonts and forbids reusing the reserved name "Plex" for
modified fonts; the embedded subsets are private data and are not distributed as font files.

The Speeduino base tunes are CC BY-NC 4.0 (non-commercial): every build embeds them, so a clibretune
bundle must not be sold unless `src/tune/basetune_data.c` is regenerated with `--empty` first.
