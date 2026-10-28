# remote_stat.sh — runs on the router via `ssh ... bash -s -- N1 PATH1 N2 PATH2 ...`.
#
# For each "N PATH" pair whose file exists prints one line:
#   PATH <tab> SIZE <tab> md5 of the first N bytes <tab> md5 of the first 64 bytes
# Read-only: uses only stat, head and md5sum.
while [ "$#" -ge 2 ]; do
  n=$1
  p=$2
  shift 2
  [ -f "$p" ] || continue
  size=$(stat -c %s -- "$p") || continue
  hn=$(head -c "$n" -- "$p" | md5sum)
  h64=$(head -c 64 -- "$p" | md5sum)
  printf '%s\t%s\t%s\t%s\n' "$p" "$size" "${hn%% *}" "${h64%% *}"
done
