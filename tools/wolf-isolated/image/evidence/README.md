Metadata acquired 2026-11-13 (UTC+07), anonymously from official endpoints only.
No OCI layers, config blobs, packages or executable binaries were downloaded.
Registry bearer token remained in acquisition-process memory and was not recorded.

- https://raw.githubusercontent.com/docker-library/official-images/master/library/archlinux
  → `official-images.txt` (trailing blank line normalized); raw acquisition SHA256 `6e143cbae08d55d06fb3378022db4759f22bf2267e978cc857efb37826f7360f`.
- https://registry-1.docker.io/v2/library/archlinux/manifests/base
  → `base-index.json`, digest `sha256:d4594bb2300cba11f41c2f27923eb56f63d9988909c402f9c95517d1e362c899`.
- https://registry-1.docker.io/v2/library/archlinux/manifests/sha256:fcee73ea1b96e236248f53a213f71276cc9072d9025b58bce9915e32f19963ae
  → `base-amd64-manifest.json`, verified against its descriptor digest and linux/amd64 index entry.
  The config descriptor is evidence only; its blob was not requested.
- https://archive.archlinux.org/repos/2026/11/12/
  → snapshot listing; repository metadata URLs/hashes and selected package records
  are in `../packages.lock.json`. Raw DB metadata is retained in the task report storage.
- https://archlinux.org/packages/extra/x86_64/gamescope/json/
  → independently reports 3.17.0-1. The frozen extra.db record, rather than this mutable
  current page, determines this source recipe's package version/checksum.

The mutable `base` tag was used only to discover a manifest. Builds use the captured
amd64 child digest. Neither the official-images label nor metadata hashes prove the
Wine dependency closure, running-image behavior, root-helper capabilities or provisioning.
