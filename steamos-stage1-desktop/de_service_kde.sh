#!/bin/bash
# de_service_kde.sh -- run the desktop service forced into stage 2 (KDE Plasma).
#
# Kept under its own name because deploy tooling and install_kde_offline.sh
# already reference it. It is now a thin wrapper: all of the bring-up,
# health-checking, fallback and logging lives in de_service.sh, which picks
# stage 2 on its own when KDE is installed (QKX_STAGE=auto).
exec env QKX_STAGE=2 /usr/local/bin/de_service.sh "$@"
