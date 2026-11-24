# FOME definitions

Upstream: https://github.com/FOME-Tech/fome-fw

What: `fome_<board>.ini` from each `fome_bundle_<board>.zip` of the latest release.

License: GPL-3.0 with the rusEFI additional terms (`LICENSE`). Copyright the FOME and rusEFI contributors. A clibretune binary containing these files is distributed under GPL-3.0.

Files are bundled unmodified; `sources.json` has the URL, commit, size and sha256 of each. Fetch them with `python3 tools/fetch_definitions.py --family fome`.

| Release | Commit | Boards |
|---------|--------|--------|
| release_260501_02 | 343fb81801 | 12: atlas, f407-discovery, hellen-nb1, hellen128, hellen72, hellenNA6, hellenNA8_96, mre_f4, mre_f7, proteus_f4, proteus_f7, proteus_h7 |
