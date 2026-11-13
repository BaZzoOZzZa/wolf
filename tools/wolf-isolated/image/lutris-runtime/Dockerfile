# Existing local tag only. Build owner must bind its before/after image ID to
# sha256:84719e0af05e1ee4720e1d6605aed0bf9c3d55d7ebf24e159aee355580632505
# and require the derived layer prefix to equal that parent's complete layer list.
# Use the default Docker driver, pull=false, network=none; never resolve/pull a new base.
FROM --platform=linux/amd64 mwow9-candidate:latest
ADD --chown=0:0 runtime-software.tar /
# Inherited UID2001 checks bytes, ownership, traversal and readability in the built filesystem.
# Bind mounts are limited to build-context metadata/tools and do not persist in the image.
RUN --mount=type=bind,source=runtime-manifest.json,target=/tmp/runtime-manifest.json,readonly \
    --mount=type=bind,source=verify.py,target=/tmp/verify-runtime.py,readonly \
    /usr/bin/python3 -I -B /tmp/verify-runtime.py
