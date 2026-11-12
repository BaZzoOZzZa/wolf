//! Read-only foundation. A valid manifest is not isolation or runtime authority.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs::{self, File},
    io::{Read, Take},
    os::unix::fs::{FileTypeExt, MetadataExt},
    path::{Component, Path},
};

pub const MAX_PROFILE: u64 = 64 * 1024;
const MAX_ARTIFACT: u64 = 256 * 1024 * 1024;
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Artifact {
    pub role: String,
    pub path: String,
    pub sha256: String,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Prefix {
    pub source: String,
    pub wine_arch: String,
    pub source_dev: u64,
    pub source_ino: u64,
    pub working: String,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub schema: u32,
    pub kind: String,
    pub desktop_uid: u32,
    pub remote_uid: u32,
    pub input_gid: u32,
    pub render_gid: u32,
    pub desktop_home: String,
    pub remote_home: String,
    pub runtime: String,
    pub prefix: Prefix,
    pub game_data: String,
    pub image: String,
    pub capabilities: Vec<String>,
    pub allow_mknod: bool,
    pub no_new_privileges: bool,
    pub private_dev: bool,
    pub host_network: bool,
    pub host_ipc: bool,
    pub artifacts: Vec<Artifact>,
    pub render_node: String,
    pub input_node: String,
}
#[derive(Debug, Serialize)]
pub struct Report {
    pub schema: u32,
    pub host: HostFacts,
    pub runtime_supported: bool,
    pub manifest_valid: bool,
    pub file_observations_valid: bool,
    pub errors: Vec<String>,
    pub unknowns: Vec<&'static str>,
    pub proposal: Option<Proposal>,
}
#[derive(Debug, Serialize)]
pub struct HostFacts {
    pub os: &'static str,
    pub architecture: &'static str,
    pub boot_id: Option<String>,
}
fn host_facts() -> HostFacts {
    let boot_id = File::open("/proc/sys/kernel/random/boot_id")
        .ok()
        .and_then(|f| {
            let mut value = String::new();
            f.take(64).read_to_string(&mut value).ok()?;
            let value = value.trim();
            (value.len() == 36 && value.bytes().all(|c| c.is_ascii_hexdigit() || c == b'-'))
                .then(|| value.to_owned())
        });
    HostFacts {
        os: std::env::consts::OS,
        architecture: std::env::consts::ARCH,
        boot_id,
    }
}
#[derive(Debug, Serialize)]
pub struct Proposal {
    pub docker_create_argv: Vec<String>,
    pub guest_argv: Vec<String>,
    pub wolf_environment: Vec<String>,
}
fn bounded_path(s: &str) -> bool {
    s.len() <= 4096
        && !s.is_empty()
        && !s.chars().any(char::is_control)
        && Path::new(s).is_absolute()
        && s != "/"
        && !s.contains("//")
        && Path::new(s)
            .components()
            .all(|c| matches!(c, Component::RootDir | Component::Normal(_)))
        && !s.split('/').any(|s| s == "." || s == "..")
        && !s.contains([',', ':'])
}
fn within(child: &str, parent: &str) -> bool {
    Path::new(child).starts_with(parent)
}
fn hex(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}
pub fn parse(bytes: &[u8]) -> Result<Profile, String> {
    if bytes.len() as u64 > MAX_PROFILE {
        return Err("profile exceeds 64 KiB".into());
    }
    serde_json::from_slice(bytes).map_err(|_| "invalid or unknown profile fields".into())
}
pub fn read_profile(path: &Path) -> Result<Profile, String> {
    // Metadata first rejects non-regular profiles. Trusted operator-owned paths only;
    // these read-only observations do not defend against concurrent administrator replacement.
    let m = fs::symlink_metadata(path).map_err(|_| "profile metadata unavailable")?;
    if !m.is_file() || m.len() > MAX_PROFILE {
        return Err("profile must be a bounded regular file".into());
    }
    let file = File::open(path).map_err(|_| "profile unavailable")?;
    let mut bytes = Vec::new();
    file.take(MAX_PROFILE + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "profile read failed")?;
    parse(&bytes)
}
pub fn validate(p: &Profile) -> Vec<String> {
    let mut e = Vec::new();
    if p.schema != 1 || p.kind != "wow-isolated-v1" {
        e.push("unsupported fixed profile".into());
    }
    if p.desktop_uid == 0
        || p.remote_uid == 0
        || p.desktop_uid == p.remote_uid
        || p.desktop_uid > 2147483646
        || p.remote_uid > 2147483646
    {
        e.push("distinct non-root UIDs required".into());
    }
    if !p.capabilities.is_empty()
        || p.allow_mknod
        || !p.no_new_privileges
        || !p.private_dev
        || p.host_network
        || p.host_ipc
    {
        e.push("unsafe guest privilege/namespace declaration".into());
    }
    if [p.input_gid, p.render_gid]
        .iter()
        .any(|g| *g == 0 || *g > 2147483646)
    {
        e.push("non-root bounded device group IDs required".into());
    }
    let paths = [
        &p.desktop_home,
        &p.remote_home,
        &p.runtime,
        &p.prefix.source,
        &p.prefix.working,
        &p.game_data,
    ];
    if paths.iter().any(|s| !bounded_path(s)) {
        e.push("invalid bounded absolute path".into());
    }
    if !within(&p.prefix.working, &p.remote_home)
        || p.prefix.working == p.remote_home
        || within(&p.remote_home, &p.desktop_home)
        || within(&p.desktop_home, &p.remote_home)
        || within(&p.prefix.working, &p.prefix.source)
        || within(&p.prefix.source, &p.prefix.working)
        || p.runtime != format!("/run/user/{}", p.remote_uid)
    {
        e.push("prefix/home/runtime separation required".into());
    }
    // Fixed mount roles, never caller-provided arbitrary host mount tables.
    for s in [&p.prefix.working, &p.game_data, &p.remote_home] {
        if ["/dev", "/proc", "/sys", "/run", "/tmp", "/var/run"]
            .iter()
            .any(|bad| within(s, bad))
            || s.ends_with(".sock")
            || s == &p.desktop_home
        {
            e.push("forbidden host mount source".into());
        }
    }
    if [&p.desktop_home, &p.remote_home, &p.prefix.source]
        .iter()
        .any(|isolated| within(&p.game_data, isolated) || within(isolated, &p.game_data))
    {
        e.push("game-data must be a separate read-only tree, not desktop home/prefix".into());
    }
    let image = p.image.split_once("@sha256:");
    if !matches!(image, Some((name,digest)) if !name.is_empty() && name.len()<=256 && name.bytes().all(|b| b.is_ascii_alphanumeric() || b"./_:-".contains(&b)) && hex(digest))
    {
        e.push("immutable image digest required".into());
    }
    let roles: BTreeSet<_> = p.artifacts.iter().map(|a| a.role.as_str()).collect();
    if p.artifacts.len() != 6
        || roles
            != BTreeSet::from([
                "wolf",
                "gamescope",
                "wine",
                "entrypoint",
                "compositor",
                "interpipe",
            ])
    {
        e.push("six unique fixed artifact roles required".into());
    }
    for a in &p.artifacts {
        if !bounded_path(&a.path) || !hex(&a.sha256) {
            e.push("invalid artifact path/digest".into());
        }
        if ["/dev", "/proc", "/sys", "/run"]
            .iter()
            .any(|b| within(&a.path, b))
        {
            e.push("artifact is not an ordinary pinned file location".into());
        }
    }
    fn numbered(s: &str, prefix: &str) -> bool {
        s.strip_prefix(prefix)
            .is_some_and(|n| !n.is_empty() && n.len() <= 5 && n.bytes().all(|b| b.is_ascii_digit()))
    }
    if !numbered(&p.render_node, "/dev/dri/renderD") || !numbered(&p.input_node, "/dev/input/event")
    {
        e.push(
            "exact render/event node paths required; no input directory/uinput/uhid mount".into(),
        );
    }
    if p.prefix.wine_arch != "win64" {
        e.push("fixed WoW profile requires win64 prefix declaration".into());
    }
    if p.prefix.source_dev == 0 || p.prefix.source_ino == 0 {
        e.push("source prefix allocation identity required".into());
    }
    e
}
fn metadata(path: &str) -> Result<fs::Metadata, String> {
    // No symlink traversal, including intermediate components. Read-only observations
    // are not an atomic start permit: the future runtime must reopen/revalidate.
    let mut part = std::path::PathBuf::new();
    for c in Path::new(path).components() {
        part.push(c);
        let m = fs::symlink_metadata(&part).map_err(|_| "missing/inaccessible path".to_string())?;
        if m.file_type().is_symlink() {
            return Err("symlink path refused".into());
        }
    }
    fs::symlink_metadata(path).map_err(|_| "metadata unavailable".into())
}
fn hash_file(path: &str) -> Result<String, String> {
    let m = metadata(path)?;
    if !m.is_file() || m.len() > MAX_ARTIFACT {
        return Err("artifact must be regular and <=256 MiB".into());
    }
    let f = File::open(path).map_err(|_| "artifact open failed")?;
    let opened = f.metadata().map_err(|_| "artifact metadata failed")?;
    if !opened.is_file() || opened.dev() != m.dev() || opened.ino() != m.ino() {
        return Err("artifact changed during inspection".into());
    }
    let mut f: Take<File> = f.take(MAX_ARTIFACT + 1);
    let mut hash = Sha256::new();
    let mut buf = [0u8; 16384];
    let mut total = 0u64;
    loop {
        let n = f.read(&mut buf).map_err(|_| "artifact read failed")?;
        if n == 0 {
            break;
        }
        total += n as u64;
        if total > MAX_ARTIFACT {
            return Err("artifact grew past bound".into());
        }
        hash.update(&buf[..n]);
    }
    if total != m.len() {
        return Err("artifact changed size during inspection".into());
    }
    Ok(format!("{:x}", hash.finalize()))
}
pub fn inspect(p: &Profile, include_plan: bool) -> Report {
    let mut errors = validate(p);
    let manifest_valid = errors.is_empty();
    if manifest_valid {
        for a in &p.artifacts {
            match hash_file(&a.path) {
                Ok(h) if h == a.sha256 => (),
                _ => errors.push(format!("artifact {} missing, changed or unpinned", a.role)),
            }
        }
        for path in [&p.remote_home, &p.runtime, &p.prefix.working] {
            match metadata(path) {
                Ok(m) if m.is_dir() && m.uid() == p.remote_uid && m.mode() & 0o077 == 0 => (),
                _ => errors.push("remote directories require exact owner and private mode".into()),
            }
        }
        match metadata(&p.prefix.source) {
            Ok(m)
                if m.is_dir()
                    && m.dev() == p.prefix.source_dev
                    && m.ino() == p.prefix.source_ino => {}
            _ => errors.push("source prefix identity mismatch/missing".into()),
        }
        if !matches!(metadata(&p.game_data),Ok(m) if m.is_dir()) {
            errors.push("game data directory missing".into());
        }
        for (node, gid, private) in [
            (&p.render_node, p.render_gid, false),
            (&p.input_node, p.input_gid, true),
        ] {
            if !matches!(metadata(node),Ok(m) if m.file_type().is_char_device() && m.gid()==gid && (!private || m.mode() & 0o007==0))
            {
                errors.push("device node type/group/private mode mismatch (metadata only)".into());
            }
        }
    }
    Report {
        schema: 1,
        host: host_facts(),
        runtime_supported: false,
        manifest_valid,
        file_observations_valid: errors.is_empty(),
        errors,
        unknowns: vec![
            "ACLs and absence of inherited device FDs",
            "image contents/dynamic loader, guest network and custom plugin provenance",
            "private display/audio and device lease generation",
            "prefix clean-stop/backup/migration approval and authentication persistence",
            "runtime ownership/locks/start/join/stop not implemented",
            "observations may race; no start authority",
        ],
        proposal: if include_plan && manifest_valid {
            Some(proposal(p))
        } else {
            None
        },
    }
}
fn proposal(p: &Profile) -> Proposal {
    let mut argv = vec![
        "docker".into(),
        "create".into(),
        "--pull=never".into(),
        "--name=wolf-wow-REQUIRES-LEASE".into(),
        "--cap-drop=ALL".into(),
        "--security-opt=no-new-privileges".into(),
        "--network=wolf-wow-egress".into(),
        "--ipc=private".into(),
        "--read-only".into(),
        format!("--user={}:{}", p.remote_uid, p.remote_uid),
        format!("--group-add={}", p.input_gid),
        format!("--group-add={}", p.render_gid),
        "--tmpfs=/tmp:rw,nosuid,nodev".into(),
    ];
    for (source, target, ro) in [
        (&p.prefix.working, "/home/wolf/prefix", false),
        (&p.game_data, "/games/wow", true),
    ] {
        argv.push("--mount".into());
        argv.push(format!(
            "type=bind,src={source},dst={target}{}",
            if ro { ",readonly" } else { "" }
        ));
    }
    // Exact socket/node mounts are future validated lease artifacts, not directory mounts.
    for (source, target) in [
        (format!("{}/wolf-wayland", p.runtime), "/run/remote/wayland"),
        (format!("{}/pulse/native", p.runtime), "/run/remote/pulse"),
    ] {
        argv.push("--mount".into());
        argv.push(format!("type=bind,src={source},dst={target}"));
    }
    for node in [&p.render_node, &p.input_node] {
        argv.push(format!("--device={node}:{node}:rw"));
    }
    argv.extend([
        "--env=WAYLAND_DISPLAY=/run/remote/wayland".into(),
        "--env=PULSE_SERVER=unix:/run/remote/pulse".into(),
        "--env=WINEPREFIX=/home/wolf/prefix".into(),
        "--env=DISPLAY=".into(),
        "--entrypoint=/opt/wolf/approved-entrypoint".into(),
        p.image.clone(),
    ]);
    Proposal {
        docker_create_argv: argv,
        guest_argv: vec![
            "/opt/wolf/approved-gamescope".into(),
            "--backend=wayland".into(),
            "--".into(),
            "/opt/wolf/approved-wine".into(),
            "/games/wow/APPROVAL_REQUIRED_BATTLENET.exe".into(),
        ],
        wolf_environment: vec![
            format!("XDG_RUNTIME_DIR={}", p.runtime),
            "DISPLAY unset; no XTEST desktop mode".into(),
            "GST_PLUGIN_PATH must name verified compositor/interpipe artifacts".into(),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::symlink;
    #[test]
    fn real_hash_symlink_missing_and_prefix_identity() {
        let root = std::env::var_os("CARGO_TARGET_DIR").expect("external target required");
        let dir = std::path::PathBuf::from(root).join(format!("fixture-{}", std::process::id()));
        fs::create_dir(&dir).unwrap();
        struct Cleanup(std::path::PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                fs::remove_dir_all(&self.0).unwrap();
            }
        }
        let _cleanup = Cleanup(dir.clone());
        let file = dir.join("artifact");
        fs::write(&file, b"abc").unwrap();
        assert_eq!(
            hash_file(file.to_str().unwrap()).unwrap(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        symlink(&file, dir.join("alias")).unwrap();
        assert!(hash_file(dir.join("alias").to_str().unwrap()).is_err());
        assert!(hash_file(dir.to_str().unwrap()).is_err());
        assert!(hash_file(dir.join("missing").to_str().unwrap()).is_err());
        let mut p = parse(include_bytes!("../profiles/wow.example.json")).unwrap();
        p.prefix.source = dir.to_str().unwrap().into();
        p.prefix.source_dev = 1;
        p.prefix.source_ino = 1;
        let r = inspect(&p, false);
        assert!(r.errors.iter().any(|e| e.contains("prefix identity")));
        p.prefix.source_dev = fs::metadata(&dir).unwrap().dev();
        p.prefix.source_ino = fs::metadata(&dir).unwrap().ino();
        let r = inspect(&p, false);
        assert!(!r.errors.iter().any(|e| e.contains("prefix identity")));
        let before = fs::read(&file).unwrap();
        let _ = inspect(&p, true);
        assert_eq!(fs::read(&file).unwrap(), before);
    }
}
