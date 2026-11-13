# SOURCE RECIPE ONLY. No runtime/provisioning approval. linux/amd64 child manifest.
FROM --platform=linux/amd64 docker.io/library/archlinux@sha256:fcee73ea1b96e236248f53a213f71276cc9072d9025b58bce9915e32f19963ae

COPY package-db.sha256 packages.requested /opt/wolf/
# Full upgrade in one frozen snapshot, with signatures and exact database bytes.
# No host Gamescope, no alternate Wine package, no host package/cache mutation.
RUN printf '%s\n' '[options]' 'Architecture = x86_64' 'ParallelDownloads = 5' \
      'SigLevel = Required DatabaseOptional' 'LocalFileSigLevel = Required' \
      '[core]' 'Server = https://archive.archlinux.org/repos/2026/11/12/core/os/$arch' \
      '[extra]' 'Server = https://archive.archlinux.org/repos/2026/11/12/extra/os/$arch' \
      '[multilib]' 'Server = https://archive.archlinux.org/repos/2026/11/12/multilib/os/$arch' \
      > /etc/pacman.conf \
    && pacman -Sy --noconfirm \
    && (cd /var/lib/pacman/sync && sha256sum -c /opt/wolf/package-db.sha256) \
    && xargs -r pacman -Su --noconfirm -- < /opt/wolf/packages.requested \
    && pacman -Q > /opt/wolf/installed-packages.txt \
    && rm -rf /var/cache/pacman/pkg/*

# Later approved build input: exact inventoried Lutris runner ONLY, never a prefix.
# No default hash: missing/unreviewed runner input must stop the build.
ARG WINE_ARCHIVE_SHA256
COPY approved-wine.tar /opt/wolf/approved-wine.tar
RUN printf '%s' "$WINE_ARCHIVE_SHA256" | grep -Eq '^[0-9a-f]{64}$' \
    && printf '%s  /opt/wolf/approved-wine.tar\n' "$WINE_ARCHIVE_SHA256" | sha256sum -c - \
    && mkdir /opt/wine \
    && tar --extract --file=/opt/wolf/approved-wine.tar --directory=/opt/wine --no-same-owner \
    && test -x /opt/wine/bin/wine && test -x /opt/wine/bin/wineserver \
    && rm /opt/wolf/approved-wine.tar \
    && chown -R 0:0 /opt/wine && chmod -R go-w /opt/wine \
    && find /opt/wine -type f -exec chmod a-s '{}' +

RUN groupadd --gid 2001 retro && useradd --uid 2001 --gid 2001 \
      --home-dir /home/retro --no-create-home --shell /usr/bin/nologin retro \
    && mkdir -p /home/retro/prefix /games/wow /run/media/bazzoozza/Games \
    && ln -s /games/wow '/run/media/bazzoozza/Games/World of Warcraft' \
    && chown -R 2001:2001 /home/retro
COPY guest.py wine-session.py approved-entrypoint /opt/wolf/
RUN chown -R 0:0 /opt/wolf && chmod -R go-w /opt/wolf \
    && chmod 0555 /opt/wolf/approved-entrypoint
USER 2001
WORKDIR /home/retro
ENV HOME=/home/retro WINEPREFIX=/home/retro/prefix
ENTRYPOINT ["/opt/wolf/approved-entrypoint"]
CMD []
# Private runtime/socket names come from RunDocker on connection.
