# Assets

Everything here is embedded into the binary at build time by generator scripts;
the build itself does not read these files.

| Path | What | License |
|------|------|---------|
| `fonts/IBMPlex*.ttf` | IBM Plex Sans (Regular, Medium, SemiBold) and IBM Plex Mono (Regular, Medium), unmodified from the official IBM/plex releases (`@ibm/plex-sans@1.1.0`, `@ibm/plex-mono@2.5.0`) | SIL OFL 1.1, `fonts/OFL.txt` |
| `icons/*.svg` | The Lucide icons used by the UI, from lucide-static 0.562.0 | ISC, `icons/LICENSE` |

Regenerate the C data:

    python3 tools/gen_embed.py                       # src/gui/fonts_data.c (subset, <=24000-byte arrays)
    python3 tools/gen_icons.py                       # src/gui/icons.h + icons_data.c
    python3 tools/gen_icons.py --fetch LUCIDE_DIR    # first re-copy SVGs from an extracted lucide-static

The embedded fonts are subset (Latin-1, Latin Extended-A, Greek, common punctuation and
symbols) by `tools/gen_embed.py`; glyph ids and kerning are unchanged. The OFL requires the
license text to travel with the fonts and forbids reusing the reserved name "Plex" for
modified fonts; the embedded subsets are private data and are not distributed as font files.
