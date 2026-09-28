#!/usr/bin/env bash
# End-to-end HLS test on the host (no console): the app's real hls.c, aseg.c,
# httpsrc.c, hls_parse.c and hls_crypt.c, built against small OpenOrbis shims,
# play scenarios served by server.py, and every byte they deliver is compared
# with the expected plaintext:
#   AES-128 (explicit IV, key rotation, sequence-number IVs, METHOD=NONE),
#   EXT-X-BYTERANGE (206, and a server that ignores Range), encrypted init +
#   ranged fMP4, a master with an encrypted data:-URI audio rendition, a
#   refetch of a bad cached key, the httpsrc->aseg init fallback, DRM refusal,
#   the TS segment-demux path, and audio-rendition choice (default, preferred
#   language, a track picked by name; other groups never offered), and the
#   streamed (sink) download the programme guide uses: Content-Length,
#   chunked and read-to-close bodies, each fetched twice on one channel, and
#   a sink that stops early; and the programme guide end to end (epg.c:
#   download, /data cache, link override, refresh, a link that is no guide),
#   channel logos (logo.c: fetch, decode, draw, eviction, broken links), and
#   free channels (freetv_net.c against a stand-in for iptv-org).
#
# Needs python3 with `cryptography` and the BearSSL tree portlibs/fetch.sh
# unpacks (BEARSSL_SRC to override). Skips (exit 0) when either is missing.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
A="$ROOT/app/src"
BEARSSL_SRC="${BEARSSL_SRC:-$ROOT/portlibs/src/bearssl-0.6}"
PY="${PYTHON:-python3}"
if [ ! -f "$BEARSSL_SRC/inc/bearssl.h" ]; then
  echo "hls-e2e: skipped (no BearSSL source at $BEARSSL_SRC; run portlibs/fetch.sh)"; exit 0
fi
if ! "$PY" -c "import cryptography" 2>/dev/null; then
  echo "hls-e2e: skipped (needs: $PY -m pip install cryptography)"; exit 0
fi
SRV=""
W="$(mktemp -d)"; trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
AES="$BEARSSL_SRC/src/symcipher"
${CC:-cc} -std=gnu11 -g -O1 -w -fsanitize=address,undefined ${E2E_CFLAGS:-} \
  -I"$HERE/shim" -I"$A" -I"$BEARSSL_SRC/inc" -I"$BEARSSL_SRC/src" -o "$W/e2e" \
  "$HERE/driver.c" "$HERE/stubs.c" "$A/hls.c" "$A/hls_parse.c" "$A/hls_crypt.c" "$A/aseg.c" \
  "$A/httpsrc.c" "$A/urlopt.c" "$A/netpolicy.c" "$A/lang.c" \
  "$AES/aes_big_cbcdec.c" "$AES/aes_big_dec.c" "$AES/aes_common.c" "$AES/aes_x86ni.c" "$AES/aes_x86ni_cbcdec.c" \
  -lpthread || { echo "hls-e2e: build failed"; exit 1; }
${CC:-cc} -std=gnu11 -g -O1 -w -fsanitize=address,undefined ${E2E_CFLAGS:-} \
  -I"$HERE/shim" -I"$A" -o "$W/epg" \
  "$HERE/epg_driver.c" "$HERE/stubs.c" "$A/epg.c" "$A/guide.c" "$A/xmltv.c" "$A/inflate.c" "$A/aseg.c" \
  "$A/hls_parse.c" "$A/httpd_channels.c" "$A/m3u.c" "$A/urlopt.c" "$A/netpolicy.c" \
  -lpthread || { echo "hls-e2e: guide build failed"; exit 1; }
${CC:-cc} -std=gnu11 -g -O1 -w -fsanitize=address,undefined ${E2E_CFLAGS:-} -DGFX_HOST_PREVIEW \
  -I"$HERE/shim" -I"$A" -o "$W/logo" \
  "$HERE/logo_driver.c" "$HERE/stubs.c" "$A/logo.c" "$A/logo_image.c" "$A/gfx.c" "$A/font_atlas.c" "$A/aseg.c" \
  "$A/hls_parse.c" "$A/httpd_channels.c" "$A/m3u.c" "$A/urlopt.c" "$A/netpolicy.c" \
  -lpthread -lm || { echo "hls-e2e: logo build failed"; exit 1; }
${CC:-cc} -std=gnu11 -g -O1 -w -fsanitize=address,undefined ${E2E_CFLAGS:-} \
  -I"$HERE/shim" -I"$A" -o "$W/freetv" \
  "$HERE/freetv_driver.c" "$HERE/stubs.c" "$A/freetv_net.c" "$A/freetv.c" "$A/aseg.c" \
  "$A/hls_parse.c" "$A/httpd_channels.c" "$A/m3u.c" "$A/urlopt.c" "$A/netpolicy.c" \
  -lpthread || { echo "hls-e2e: freetv build failed"; exit 1; }
"$PY" "$HERE/server.py" "$W/out" 0 >"$W/server.err" 2>&1 & SRV=$!
for _ in $(seq 50); do [ -s "$W/out/port" ] && break; sleep 0.1; done
[ -s "$W/out/port" ] || { echo "hls-e2e: server did not start"; cat "$W/server.err"; exit 1; }
U="http://127.0.0.1:$(cat "$W/out/port")"; O="$W/out"
export ASAN_OPTIONS=detect_leaks=0
fail=0
run() {   # name, env, args...
  local name="$1" envs="$2"; shift 2
  if out=$(env $envs "$W/e2e" "$@" 2>&1); then echo "ok   $name"
  else echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -12; fail=1; fi
}
run "aes-128 vod"                "" "$U/aes/index.m3u8"  "$O/aes.bin"
run "byte ranges (206)"          "" "$U/br/index.m3u8"   "$O/br.bin"
run "byte ranges (Range ignored)" "" "$U/brx/index.m3u8" "$O/br.bin"
run "encrypted init + ranged fmp4" "" "$U/efm/index.m3u8" "$O/efm.bin"
run "master + encrypted audio"   "" "$U/ma/master.m3u8"  "$O/aes.bin" "$O/ma_audio.bin"
run "plain"                      "" "$U/plain/index.m3u8" "$O/plain.bin"
run "bad cached key refetched"   "" "$U/rot/index.m3u8"  "$O/rot.bin"
run "init via aseg fallback"     "" "$U/fb/index.m3u8"   "$O/fb.bin"
run "drm refused"                "" "$U/drm/index.m3u8"  -
run "segdemux aes-128"           "SEGDEMUX=1" "$U/aes/index.m3u8" "$O/aes.bin"
run "audio: default rendition"   "EXPECT_RENDS=3" "$U/ml/master.m3u8" "$O/plain.bin" "$O/ml_en.bin"
run "audio: preferred language"  "EXPECT_RENDS=3 APREF_LANG=fra" "$U/ml/master.m3u8" "$O/plain.bin" "$O/ml_fr.bin"
run "audio: picked by name"      "EXPECT_RENDS=3 APREF_NAME=Deutsch APREF_LANG=fra" "$U/ml/master.m3u8" "$O/plain.bin" "$O/ml_de.bin"
run "stream: content-length"     "STREAM=1" "$U/stream/known.bin" "$O/stream.bin"
run "stream: chunked"            "STREAM=1" "$U/stream/chunked.bin" "$O/stream.bin"
run "stream: to close"           "STREAM=1" "$U/stream/eof.bin" "$O/stream.bin"
run "stream: sink stops"         "STREAM=1 STREAM_STOP=100000" "$U/stream/chunked.bin" "$O/stream.bin"
mkdir -p "$W/data"
if out=$(PS4CAST_DATA="$W/data" "$W/epg" "$U" "$O/server.log" 2>&1); then echo "ok   guide: download, cache, override, refresh"
else echo "FAIL guide: download, cache, override, refresh"; echo "$out" | sed 's/^/     /' | head -12; fail=1; fi
rm -rf "$W/data2"; mkdir -p "$W/data2"
if out=$(PS4CAST_DATA="$W/data2" "$W/logo" "$U" 2>&1); then echo "ok   logos: fetch, decode, draw, evict"
else echo "FAIL logos: fetch, decode, draw, evict"; echo "$out" | sed 's/^/     /' | head -12; fail=1; fi
rm -rf "$W/data3"; mkdir -p "$W/data3"
if out=$(PS4CAST_DATA="$W/data3" "$W/freetv" "$U" 2>&1); then echo "ok   free channels: download, add, add again"
else echo "FAIL free channels: download, add, add again"; echo "$out" | sed 's/^/     /' | head -12; fail=1; fi
[ "$fail" = 0 ] && echo "hls-e2e: all ok" || echo "hls-e2e: FAILURES"
exit $fail
