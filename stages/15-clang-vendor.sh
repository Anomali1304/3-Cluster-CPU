#!/usr/bin/env bash
set -e

_extract_clang_archive() {
    local archive="$1" dest="$2"
    case "$archive" in
        *.tar.zst|*.zst)
            check_cmd zstd || error "zstd not installed — required to extract $archive (see stages/00-deps.sh)."
            tar --zstd -xf "$archive" -C "$dest"
            ;;
        *.tar.xz|*.xz) tar -xJf "$archive" -C "$dest" ;;
        *)             tar -xf "$archive" -C "$dest" ;;
    esac
}

case "${CLANG_VENDOR:-GKI}" in
    GKI)
        log "CLANG_VENDOR=GKI — using the clang GKI's build/build.sh brings on its own."
        ;;

    ZyC)
        log "CLANG_VENDOR=ZyC — resolving latest ZyCromerZ/Clang release..."
        check_cmd jq || error "jq not installed (see stages/00-deps.sh)."

        # Prefer the Releases API, but keep a non-API fallback because GitHub can
        # rate-limit unauthenticated API requests from Actions runners.
        release_json=""
        if release_json="$(curl -fsSL --retry 3 --retry-delay 2 \
            -H 'Accept: application/vnd.github+json' \
            -H 'User-Agent: 3-Cluster-CPU-build' \
            https://api.github.com/repos/ZyCromerZ/Clang/releases/latest)"; then
            api_message="$(printf '%s' "$release_json" | jq -r '.message // empty' 2>/dev/null || true)"
            if [ -z "$api_message" ]; then
                asset_url="$(printf '%s' "$release_json" \
                    | jq -r '(.assets // [])[]?.browser_download_url // empty' \
                    | grep -E 'Clang-.*\.(tar\.gz|tar\.zst|tar\.xz)$' \
                    | sort -V | tail -1)"
            else
                warn "ZyC Releases API returned: $api_message — trying release-page fallback."
            fi
        else
            warn "ZyC Releases API unavailable — trying release-page fallback."
        fi

        if [ -z "${asset_url:-}" ]; then
            asset_url="$(curl -fsSL --retry 3 --retry-delay 2 \
                -H 'User-Agent: 3-Cluster-CPU-build' \
                https://github.com/ZyCromerZ/Clang/releases/latest \
                | grep -oE 'https://github\.com/ZyCromerZ/Clang/releases/download/[^"< ]+/Clang-[^"< ]+\.(tar\.gz|tar\.zst|tar\.xz)' \
                | head -1 || true)"
        fi

        # Last-resort pinned release known to publish a complete Clang tarball.
        if [ -z "${asset_url:-}" ]; then
            asset_url="https://github.com/ZyCromerZ/Clang/releases/download/23.0.0git-20260130-release/Clang-23.0.0git-20260130.tar.gz"
            warn "Using pinned ZyC Clang fallback: $asset_url"
        fi
        [ -n "$asset_url" ] || error "No matching Clang asset found in ZyCromerZ/Clang latest release."
        log "ZyC Clang asset: $asset_url"

        target_dir="$WORKSPACE/clang-zyc"
        rm -rf "$target_dir"; mkdir -p "$target_dir"
        tmp_dir="$(mktemp -d)"
        filename="$(basename "${asset_url%%\?*}")"
        [ -n "$filename" ] || filename="clang.tar"
        wget -q "$asset_url" -O "$tmp_dir/$filename" \
            || { rm -rf "$tmp_dir"; error "ZyC Clang download failed."; }
        _extract_clang_archive "$tmp_dir/$filename" "$target_dir" \
            || { rm -rf "$tmp_dir" "$target_dir"; error "ZyC Clang extract failed."; }
        rm -rf "$tmp_dir"

        [ -x "$target_dir/bin/clang" ] || { rm -rf "$target_dir"; error "Extracted ZyC Clang has no bin/clang."; }
        CLANG_CUSTOM_PATH="$target_dir"
        export CLANG_CUSTOM_PATH
        ok "ZyC Clang $(get_clang_version) ready at $CLANG_CUSTOM_PATH"
        ;;

    Custom)
        if [ -x "${CLANG_CUSTOM_PATH}/bin/clang" ]; then
            ok "CLANG_VENDOR=Custom — reusing existing toolchain at $CLANG_CUSTOM_PATH"
        elif [ -n "${CLANG_CUSTOM_URL:-}" ]; then
            log "CLANG_VENDOR=Custom — downloading $CLANG_CUSTOM_URL ..."
            target_dir="$WORKSPACE/clang-custom"
            rm -rf "$target_dir"; mkdir -p "$target_dir"
            tmp_dir="$(mktemp -d)"
            filename="$(basename "${CLANG_CUSTOM_URL%%\?*}")"
            [ -n "$filename" ] || filename="clang.tar"
            wget -q "$CLANG_CUSTOM_URL" -O "$tmp_dir/$filename" \
                || { rm -rf "$tmp_dir"; error "Custom Clang download failed."; }
            _extract_clang_archive "$tmp_dir/$filename" "$target_dir" \
                || { rm -rf "$tmp_dir" "$target_dir"; error "Custom Clang extract failed."; }
            rm -rf "$tmp_dir"
            [ -x "$target_dir/bin/clang" ] || { rm -rf "$target_dir"; error "Extracted Custom Clang has no bin/clang."; }
            CLANG_CUSTOM_PATH="$target_dir"
            export CLANG_CUSTOM_PATH
            ok "Custom Clang $(get_clang_version) ready at $CLANG_CUSTOM_PATH"
        else
            error "CLANG_VENDOR=Custom requires CLANG_CUSTOM_PATH (existing bin/clang) or CLANG_CUSTOM_URL (archive to download)."
        fi
        ;;

    *)
        error "Unknown CLANG_VENDOR=${CLANG_VENDOR} (expected GKI, ZyC, or Custom)."
        ;;
esac
