# split_ini.awk - cut an INI into one file per [Section] (pieces of at most ~24 KB, each
# starting with its section header) so the fuzzer gets small, realistic seeds.
#   awk -v out=PREFIX -f tools/fuzz/split_ini.awk FILE.ini   -> PREFIX_1, PREFIX_2, ...
BEGIN { n = 0; sz = 0; hdr = "" }
/^\[[A-Za-z]/ { n++; sz = 0; hdr = $0 }
{
    if (sz > 24000) { n++; sz = 0; print hdr > (out "_" n) }
    print > (out "_" n)
    sz += length($0) + 1
}
