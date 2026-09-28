# Host-side unit tests: pure logic compiled and run on macOS, no console needed.
#   make test        build + run everything
#   make clean
CC ?= cc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -g -O1 -std=gnu11

SRC = ../../app/src

TESTS = test_hls_parse test_urlopt test_resolve test_qr test_openq test_token test_netpolicy test_netwatch test_lang test_m3u test_channels test_inflate test_xmltv test_guide test_logo_image test_subs test_agent_crypto

# HLS AES-128 decryption (app/src/hls_crypt.c) against BearSSL's AES, built
# from the pinned source tree that portlibs/fetch.sh unpacks. Skipped (with a
# note) when that tree isn't there.
BEARSSL_SRC ?= ../../portlibs/src/bearssl-0.6
BEARSSL_AES = $(addprefix $(BEARSSL_SRC)/src/symcipher/,aes_big_cbcdec.c aes_big_dec.c aes_common.c aes_x86ni.c aes_x86ni_cbcdec.c)
ifneq ($(wildcard $(BEARSSL_SRC)/inc/bearssl.h),)
TESTS += test_hls_crypt
else
SKIP_CRYPT = test_hls_crypt: skipped (no BearSSL source at $(BEARSSL_SRC); run portlibs/fetch.sh)
endif

# Audio-track choice/labels (app/src/audiotrack.c) against a HOST FFmpeg
# (the app's portlibs build is for the PS4). Point FFMPEG_HOST at an install
# prefix with the aac + ac3 decoders, e.g. `brew --prefix ffmpeg`, or a
# minimal ffmpeg-6.1.6 `./configure --prefix=... --disable-everything
# --disable-programs --enable-decoder=aac,ac3,mpeg4 --enable-demuxer=matroska,mov,mpegts`.
FFMPEG_HOST ?=
ifneq ($(FFMPEG_HOST),)
TESTS += test_audiotrack
FFMPEG_LIBS ?= $(or $(shell PKG_CONFIG_PATH=$(FFMPEG_HOST)/lib/pkgconfig pkg-config --libs --static libavformat libavcodec libavutil 2>/dev/null),-L$(FFMPEG_HOST)/lib -lavformat -lavcodec -lswresample -lavutil -lm -lpthread)
else
SKIP_ATRACK = test_audiotrack: skipped (set FFMPEG_HOST to a host FFmpeg install prefix)
endif

all: $(TESTS)

test_hls_parse: test_hls_parse.c $(SRC)/hls_parse.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_hls_parse.c $(SRC)/hls_parse.c

test_urlopt: test_urlopt.c $(SRC)/urlopt.c $(SRC)/netpolicy.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_urlopt.c $(SRC)/urlopt.c $(SRC)/netpolicy.c

# resolve.c's interesting half is static; include it directly.
test_resolve: test_resolve.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_resolve.c

test_qr: test_qr.c $(SRC)/qr.c $(SRC)/qrcodegen.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_qr.c $(SRC)/qr.c $(SRC)/qrcodegen.c

# Opener worker's latest-wins request slot (app/src/openq.c).
test_openq: test_openq.c $(SRC)/openq.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_openq.c $(SRC)/openq.c

# Private-host detection, cookie scope, build-date parsing (app/src/netpolicy.c).
test_netpolicy: test_netpolicy.c $(SRC)/netpolicy.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_netpolicy.c $(SRC)/netpolicy.c

# Address appeared/changed/lost detector (app/src/netwatch.c).
test_netwatch: test_netwatch.c $(SRC)/netwatch.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_netwatch.c $(SRC)/netwatch.c

# M3U/IPTV parsing and per-channel options, through urlopt into headers.
test_m3u: test_m3u.c $(SRC)/m3u.c $(SRC)/urlopt.c $(SRC)/netpolicy.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_m3u.c $(SRC)/m3u.c $(SRC)/urlopt.c $(SRC)/netpolicy.c

# Channel store: big lists, rail/filters, save + reload, /channel/* (app/src/httpd_channels.c).
# shim/ stands in for the two OpenOrbis headers it includes.
test_channels: test_channels.c $(SRC)/httpd_channels.c $(SRC)/m3u.c $(SRC)/urlopt.c $(SRC)/netpolicy.c shim/orbis/libkernel.h
	$(CC) $(CFLAGS) -Ishim -I$(SRC) -o $@ test_channels.c $(SRC)/httpd_channels.c $(SRC)/m3u.c $(SRC)/urlopt.c $(SRC)/netpolicy.c -lpthread

# Streaming gzip/zlib decoder (app/src/inflate.c). inflate_vectors.h comes
# from gen_inflate_vectors.py (Python's zlib), so the check is against zlib.
test_inflate: test_inflate.c inflate_vectors.h $(SRC)/inflate.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_inflate.c $(SRC)/inflate.c

# XMLTV guide parser (app/src/xmltv.c).
test_xmltv: test_xmltv.c $(SRC)/xmltv.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_xmltv.c $(SRC)/xmltv.c

# Programme-guide store: matching, window, now/next (app/src/guide.c).
test_guide: test_guide.c $(SRC)/guide.c $(SRC)/xmltv.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_guide.c $(SRC)/guide.c $(SRC)/xmltv.c

# Channel-logo decoding and shrinking (app/src/logo_image.c, stb_image).
# logo_vectors.h comes from gen_logo_vectors.py (Pillow).
test_logo_image: test_logo_image.c logo_vectors.h $(SRC)/logo_image.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_logo_image.c $(SRC)/logo_image.c -lm

# Subtitle parsing, clean-up, wrapping and cue list (app/src/subs.c).
test_subs: test_subs.c $(SRC)/subs.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_subs.c $(SRC)/subs.c

# Audio-language normalisation and matching (app/src/lang.c).
test_lang: test_lang.c $(SRC)/lang.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_lang.c $(SRC)/lang.c

# Deploy agent's SHA-256/HMAC and wire parser (payloads/ps4cast-dpi).
AGENT = ../../payloads/ps4cast-dpi
test_agent_crypto: test_agent_crypto.c $(AGENT)/sha256.c $(AGENT)/agent_proto.c
	$(CC) $(CFLAGS) -I$(AGENT) -o $@ test_agent_crypto.c $(AGENT)/sha256.c $(AGENT)/agent_proto.c

# Pairing token alphabet/validator/derivation (app/src/pairing.c).
test_token: test_token.c $(SRC)/pairing.c
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_token.c $(SRC)/pairing.c

test_audiotrack: test_audiotrack.c $(SRC)/audiotrack.c $(SRC)/lang.c
	$(CC) $(CFLAGS) -I$(SRC) -I$(FFMPEG_HOST)/include -o $@ test_audiotrack.c $(SRC)/audiotrack.c $(SRC)/lang.c $(FFMPEG_LIBS)

test_hls_crypt: test_hls_crypt.c $(SRC)/hls_crypt.c
	$(CC) $(CFLAGS) -I$(SRC) -I$(BEARSSL_SRC)/inc -I$(BEARSSL_SRC)/src -o $@ test_hls_crypt.c $(SRC)/hls_crypt.c $(BEARSSL_AES)

test: all
	@for t in $(TESTS); do ./$$t || exit 1; done
	@$(if $(SKIP_CRYPT),echo "$(SKIP_CRYPT)",true)
	@$(if $(SKIP_ATRACK),echo "$(SKIP_ATRACK)",true)
	@./test_qr > /tmp/qr_now.txt
	@golden=./test_qr_golden.txt; \
	 [ -f "$$golden" ] || golden=../../tests/host/test_qr_golden.txt; \
	 cmp -s /tmp/qr_now.txt "$$golden" && echo "test_qr: goldens match" || { echo "test_qr: GOLDEN MISMATCH"; diff "$$golden" /tmp/qr_now.txt | head; exit 1; }

clean:
	rm -f $(TESTS) test_hls_crypt test_audiotrack

.PHONY: all test clean
