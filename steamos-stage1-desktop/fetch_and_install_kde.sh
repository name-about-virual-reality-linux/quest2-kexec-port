#!/bin/bash
# fetch_and_install_kde.sh: Fast, robust curl-based package fetcher & installer
set -e

echo "==> Step 1: Getting package list from pacman..."
rm -f /var/lib/pacman/db.lck
mkdir -p /var/cache/pacman/pkg

URLS=$(pacman -Sp --print-format "%u" \
    plasma-desktop \
    plasma-workspace \
    plasma-x11-session \
    kwin-x11 \
    plasma-welcome \
    konsole \
    dolphin \
    breeze \
    breeze-icons \
    noto-fonts \
    noto-fonts-emoji 2>/dev/null | grep -E '^http|^file')

TOTAL=$(echo "$URLS" | wc -l)
echo "==> Total packages to verify: $TOTAL"

COUNT=0
DOWNLOADED=0

for URL in $URLS; do
    COUNT=$((COUNT + 1))
    FNAME=$(basename "$URL")
    DEST="/var/cache/pacman/pkg/$FNAME"
    if [ -f "$DEST" ] && [ -s "$DEST" ]; then
        continue
    fi
    # Download via curl over local USB link
    curl -s -L -C - -o "$DEST" "$URL"
    DOWNLOADED=$((DOWNLOADED + 1))
    if [ $((COUNT % 20)) -eq 0 ] || [ $COUNT -eq $TOTAL ]; then
        echo "==> Verified $COUNT / $TOTAL ($DOWNLOADED downloaded)"
    fi
done

echo "==> All $TOTAL packages cached in /var/cache/pacman/pkg!"
echo "==> Step 2: Installing full KDE Plasma 6 Desktop and OOBE Suite..."

pacman -S --noconfirm --noprogressbar \
    plasma-desktop \
    plasma-workspace \
    plasma-x11-session \
    kwin-x11 \
    plasma-welcome \
    konsole \
    dolphin \
    breeze \
    breeze-icons \
    noto-fonts \
    noto-fonts-emoji

echo "==> KDE Plasma 6 Desktop installation complete!"
