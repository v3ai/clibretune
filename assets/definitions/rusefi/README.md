# rusEFI definitions

Upstream: https://github.com/rusefi/rusefi

What: `rusefi_<board>.ini` from each `rusefi_bundle_<board>.zip` of the pinned releases (the `_autoupdate`, `_debug` and `_obfuscated` bundles are skipped; bundles whose INI is identical to another board's are listed under `same_as`).

License: GPL-3.0 with the additional terms of the rusEFI `license.txt`. Copyright rusEFI LLC and contributors. A clibretune binary containing these files is distributed under GPL-3.0 (see `../README.md`).

Files are bundled unmodified; `sources.json` has the URL, commit, size and sha256 of each. Fetch them with `python3 tools/fetch_definitions.py --family rusefi`.

| Release | Commit | Boards |
|---------|--------|--------|
| release_20260812 | a6d6d77b88 | 47: alphax-2chan, alphax-4chan, alphax-4chan_f7, alphax-4k-gdi, alphax-8chan, alphax-8chan_f7, alphax-gold, alphax-silver, alphax-silver-a, canBridge, f407-discovery, f429-discovery, frankenso_na6, hellen-gm-e67, hellen-honda-k, hellen-nb1, hellen121nissan, hellen121vag, hellen128, hellen154hyundai, hellen154hyundai_f7, hellen72, hellenNA6, hellenNA8_96, mega100, mega144-f7, mre-legacy_f4, mre_f4, mre_f7, nano, nissan-z31, proteus_f4, proteus_f7, proteus_h7, r-ecu, small-can-board, stm32f429_nucleo, stm32f767_nucleo, stm32h743_nucleo, subaru_eg33_f7 (= subaru_eg33_f7_no_bl), super-uaefi, t-b-g, uaefi, uaefi-obd1, uaefi121, uaefi_pro, uaefi_pro_h7 |
| release_20260220_2 | 11cc73c311 | 39: alphax-2chan, alphax-4chan, alphax-4chan_f7, alphax-4k-gdi, alphax-8chan, alphax-8chan_f7, alphax-gold, alphax-silver, alphax-silver-a, f407-discovery, f429-discovery, frankenso_na6, hellen-gm-e67, hellen-honda-k, hellen-nb1, hellen121nissan, hellen121vag, hellen128, hellen154hyundai, hellen154hyundai_f7, hellen72, hellenNA6, hellenNA8_96, mre-legacy_f4, mre_f4, mre_f7, proteus_f4, proteus_f7, proteus_h7, small-can-board, stm32f767_nucleo, stm32h743_nucleo, subaru_eg33_f7 (= subaru_eg33_f7_no_bl), super-uaefi, t-b-g, uaefi, uaefi121, uaefi_pro, uaefi_pro_h7 |
| release_20251018 | 6f495091cf | 37: alphax-2chan, alphax-4chan, alphax-4chan_f7, alphax-4k-gdi, alphax-8chan, alphax-8chan_f7, alphax-gold, alphax-silver, alphax-silver-a, f407-discovery, f429-discovery, frankenso_na6, hellen-honda-k, hellen-nb1, hellen121nissan, hellen121vag, hellen128, hellen154hyundai, hellen154hyundai_f7, hellen72, hellenNA6, hellenNA8_96, mre-legacy_f4, mre_f4, mre_f7, proteus_f4, proteus_f7, proteus_h7, small-can-board, stm32f767_nucleo, stm32h743_nucleo, subaru_eg33_f7 (= subaru_eg33_f7_no_bl), super-uaefi, t-b-g, uaefi, uaefi121, uaefi_pro |
