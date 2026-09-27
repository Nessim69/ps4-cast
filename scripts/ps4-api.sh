#!/usr/bin/env bash
# Shared PS4 Cast HTTP helpers. Source from dev scripts:
#   source "$(dirname "$0")/ps4-api.sh"
#   ps4_post play "http://..."        -> POST /play with the pairing token
#   ps4_post stop
# The receiver gates state-changing endpoints (now including GET /token
# itself) behind the pairing token shown on the TV; /status is read-only and
# deliberately does NOT expose it. ps4_token() below resolves one from, in
# order: $PS4CAST_TOKEN, a gitignored .ps4cast-token file at the repo root, or
# GET /token (which only answers while the console's pairing window is open --
# press Square on PS4 Cast's Cast home screen).
PS4=${PS4:-192.168.1.4}

ps4_token() {
    if [ -n "${PS4CAST_TOKEN:-}" ]; then
        printf '%s' "$PS4CAST_TOKEN"
        return
    fi
    local root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [ -s "$root/.ps4cast-token" ]; then
        local fileTok
        fileTok="$(tr -d '[:space:]' < "$root/.ps4cast-token")"
        if [[ "$fileTok" =~ ^[A-Z2-9]{8}$ ]]; then
            printf '%s' "$fileTok"
            return
        fi
    fi
    local tok
    tok="$(curl -sS -m3 "http://$PS4:8080/token" 2>/dev/null)"
    # curl doesn't fail on a 401 (that's an HTTP-level status, not a transport
    # error), so without this shape check a closed pairing window would
    # silently return its error BODY as if it were the token.
    if [[ "$tok" =~ ^[A-Z2-9]{8}$ ]]; then
        printf '%s' "$tok"
        return
    fi
    echo "no pairing token: set PS4CAST_TOKEN, write it to .ps4cast-token at the repo root, or press Square on PS4 Cast's Cast home screen to open the 2-minute pairing window" >&2
}

ps4_post() {  # ps4_post <endpoint-without-slash> [curl args...]
    local ep="$1"; shift
    local tok; tok="$(ps4_token)"
    local q=""
    [ -n "$tok" ] && q="?t=$tok"
    curl -sS -m5 -X POST "http://$PS4:8080/$ep$q" "$@"
}

ps4_status() { curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null; }
