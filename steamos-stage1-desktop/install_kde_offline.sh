#!/bin/bash
# install_kde_offline.sh: Stream and install all KDE Plasma 6 + OOBE packages
set -e

echo "==> Step 1: Clearing stale locks..."
rm -f /var/lib/pacman/db.lck

echo "==> Step 2: Streaming 1.3GB package archive from host over high-speed USB..."
mkdir -p /var/cache/pacman/pkg
curl -s http://10.42.0.1:8000/kde-full-pkgs.tar | tar -xf - -C /var/cache/pacman/pkg/

echo "==> Step 3: Verified package cache count:"
ls /var/cache/pacman/pkg/*.pkg.tar.zst | wc -l

echo "==> Step 4: Installing KDE Plasma 6 Desktop, KWin, OOBE Wizard, Konsole, and Dolphin..."
pacman -S --noconfirm --noprogressbar \
    plasma-desktop \
    plasma-workspace \
    plasma-x11-session \
    kwin-x11 \
    plasma-welcome \
    plasma-workspace-wallpapers \
    konsole \
    dolphin \
    breeze \
    breeze-icons \
    noto-fonts \
    noto-fonts-emoji

echo "==> Step 5: Setting permissions and deploying services..."
chmod +x /usr/local/bin/de_service_kde.sh
chmod +x /usr/local/bin/mirror_xwd.py

echo "==> KDE Plasma 6 installation complete!"
