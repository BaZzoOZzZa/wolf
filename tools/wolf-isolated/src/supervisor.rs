//! Linux foreground child owner. Docker protocol here is GET-only. No tests start this backend.
use crate::{runtime::*, runtime_spec::*, Profile};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::{
            fs::{FileTypeExt, MetadataExt, OpenOptionsExt},
            net::{UnixListener, UnixStream},
        },
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};
unsafe extern "C" {
    fn geteuid() -> u32;
    fn flock(fd: i32, op: i32) -> i32;
    fn pidfd_open(pid: i32, flags: u32) -> i32;
    fn pidfd_send_signal(fd: i32, sig: i32, info: *const std::ffi::c_void, flags: u32) -> i32;
    fn getrandom(buf: *mut std::ffi::c_void, len: usize, flags: u32) -> isize;
    fn signal(sig: i32, handler: usize) -> usize;
}
static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn stop_signal(_: i32) {
    STOP.store(true, Ordering::Relaxed);
}
fn err() -> String {
    "runtime refused/unknown; private journal retained when occupied".into()
}
pub struct NativeSource;
impl Source for NativeSource {
    fn root_file(&mut self, path: &str, limit: u64) -> Result<Record, String> {
        let m = crate::metadata(path)?;
        if !m.is_file() || m.len() > limit {
            return Err(err());
        }
        let f = File::open(path).map_err(|_| err())?;
        let n = f.metadata().map_err(|_| err())?;
        if !n.is_file() || n.dev() != m.dev() || n.ino() != m.ino() || n.len() > limit {
            return Err(err());
        }
        let mut bytes = Vec::new();
        f.take(limit + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| err())?;
        if bytes.len() as u64 > limit {
            return Err(err());
        }
        Ok(Record {
            bytes,
            uid: n.uid(),
            mode: n.mode(),
        })
    }
    fn private_dir(&mut self, path: &str, uid: u32) -> Result<(u64, u64), String> {
        let m = crate::metadata(path)?;
        if !m.is_dir() || m.uid() != uid || m.mode() & 0o077 != 0 {
            return Err(err());
        }
        Ok((m.dev(), m.ino()))
    }
    fn socket(&mut self, path: &str, uid: Option<u32>) -> Result<(), String> {
        let m = crate::metadata(path)?;
        if !m.file_type().is_socket() || uid.is_some_and(|u| m.uid() != u) {
            return Err(err());
        }
        Ok(())
    }
    fn device(&mut self, path: &str, gid: u32) -> Result<(), String> {
        let m = crate::metadata(path)?;
        if !m.file_type().is_char_device()
            || m.gid() != gid
            || m.mode() & 0o007 != 0
            || m.mode() & 0o060 != 0o060
        {
            return Err(err());
        }
        Ok(())
    }
    fn groups(&mut self) -> Result<Vec<u32>, String> {
        let text = fs::read_to_string("/proc/self/status").map_err(|_| err())?;
        let mut out = Vec::new();
        for line in text.lines() {
            if line.starts_with("Groups:") || line.starts_with("Gid:") {
                for token in line.split_whitespace().skip(1) {
                    if out.len() >= 128 {
                        return Err(err());
                    }
                    out.push(token.parse().map_err(|_| err())?);
                }
            }
        }
        Ok(out)
    }
    fn boot_id(&mut self) -> Result<String, String> {
        boot()
    }
    fn process_live(&mut self, id: &ChildId) -> Result<bool, String> {
        match fs::symlink_metadata(format!("/proc/{}", id.pid)) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(false),
            Err(_) => Err(err()),
            Ok(_) => Ok(process_start(id.pid)? == id.start),
        }
    }
    fn uid(&self) -> u32 {
        unsafe { geteuid() }
    }
    fn directory_identity(&mut self, path: &str) -> Result<(u64, u64), String> {
        let m = crate::metadata(path)?;
        if !m.is_dir() {
            return Err(err());
        }
        Ok((m.dev(), m.ino()))
    }
    fn gpu_vendor(&mut self, node: &str) -> Result<String, String> {
        let name = Path::new(node)
            .file_name()
            .ok_or_else(err)?
            .to_str()
            .ok_or_else(err)?;
        let f = File::open(format!("/sys/class/drm/{name}/device/vendor")).map_err(|_| err())?;
        let mut b = String::new();
        f.take(32).read_to_string(&mut b).map_err(|_| err())?;
        match b.trim() {
            "0x8086" => Ok("INTEL".into()),
            "0x1002" => Ok("AMD".into()),
            _ => Err(err()),
        }
    }
}
fn private_file(path: &Path, uid: u32, limit: u64) -> Result<Vec<u8>, String> {
    let m = crate::metadata(path.to_str().ok_or_else(err)?)?;
    if !m.is_file() || m.uid() != uid || m.mode() & 0o077 != 0 || m.len() > limit {
        return Err(err());
    }
    let f = File::open(path).map_err(|_| err())?;
    let n = f.metadata().map_err(|_| err())?;
    if n.dev() != m.dev() || n.ino() != m.ino() {
        return Err(err());
    }
    let mut b = Vec::new();
    f.take(limit + 1).read_to_end(&mut b).map_err(|_| err())?;
    if b.len() as u64 > limit {
        return Err(err());
    }
    Ok(b)
}
fn exclusive(path: &Path, uid: u32) -> Result<File, Fault> {
    if let Ok(m) = fs::symlink_metadata(path) {
        if !m.is_file() || m.uid() != uid || m.mode() & 0o077 != 0 {
            return Err(Fault);
        }
    }
    let f = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .open(path)
        .map_err(|_| Fault)?;
    if unsafe { flock(f.as_raw_fd(), 2 | 4) } != 0 {
        return Err(Fault);
    }
    Ok(f)
}
fn atomic_write(dir: &Path, name: &str, bytes: &[u8]) -> Result<(), Fault> {
    let tmp = dir.join(format!(".{name}.tmp"));
    let mut f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&tmp)
        .map_err(|_| Fault)?;
    let result = (|| {
        f.write_all(bytes).map_err(|_| Fault)?;
        f.sync_all().map_err(|_| Fault)?;
        fs::rename(&tmp, dir.join(name)).map_err(|_| Fault)?;
        File::open(dir)
            .and_then(|f| f.sync_all())
            .map_err(|_| Fault)
    })();
    if result.is_err() {
        let _ = fs::remove_file(tmp);
    }
    result
}
fn boot() -> Result<String, String> {
    let b = fs::read_to_string("/proc/sys/kernel/random/boot_id").map_err(|_| err())?;
    let b = b.trim();
    if b.len() != 36 {
        return Err(err());
    }
    Ok(b.into())
}
fn process_start(pid: u32) -> Result<u64, String> {
    let s = fs::read_to_string(format!("/proc/{pid}/stat")).map_err(|_| err())?;
    // comm may contain spaces/parentheses: fields following the LAST ')' start at state (3).
    s.rsplit_once(')')
        .and_then(|(_, tail)| tail.split_whitespace().nth(19))
        .and_then(|s| s.parse().ok())
        .ok_or_else(err)
}
fn nonce() -> Result<String, String> {
    let mut b = [0u8; 32];
    let mut pos = 0;
    while pos < b.len() {
        let n = unsafe { getrandom(b[pos..].as_mut_ptr().cast(), b.len() - pos, 0) };
        if n <= 0 {
            return Err(err());
        }
        pos += n as usize;
    }
    Ok(b.iter().map(|b| format!("{b:02x}")).collect())
}
fn escape(s: &str) -> String {
    s.bytes()
        .map(|b| {
            if b.is_ascii_alphanumeric() || b"-_.~".contains(&b) {
                (b as char).to_string()
            } else {
                format!("%{b:02X}")
            }
        })
        .collect()
}
fn get(socket: &str, path: &str) -> Result<Value, Fault> {
    if !path.starts_with("/v1.40/") || path.bytes().any(|b| b == b'\r' || b == b'\n') {
        return Err(Fault);
    }
    let mut s = UnixStream::connect(socket).map_err(|_| Fault)?;
    s.set_read_timeout(Some(Duration::from_millis(500)))
        .map_err(|_| Fault)?;
    s.set_write_timeout(Some(Duration::from_millis(500)))
        .map_err(|_| Fault)?;
    write!(
        s,
        "GET {path} HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )
    .map_err(|_| Fault)?;
    let mut b = Vec::new();
    s.take(256 * 1024 + 1)
        .read_to_end(&mut b)
        .map_err(|_| Fault)?;
    if b.len() > 256 * 1024 {
        return Err(Fault);
    }
    let split = b.windows(4).position(|s| s == b"\r\n\r\n").ok_or(Fault)?;
    let h = std::str::from_utf8(&b[..split]).map_err(|_| Fault)?;
    if !h
        .lines()
        .next()
        .is_some_and(|s| s.split_whitespace().nth(1) == Some("200"))
        || h.to_ascii_lowercase().contains("transfer-encoding:")
    {
        return Err(Fault);
    }
    serde_json::from_slice(&b[split + 4..]).map_err(|_| Fault)
}
/// Decode the real Docker GET inspect path; uncertain/dead is never an exit receipt.
pub fn decode_inspection(d: &Value, id: &str) -> Result<Guest, Fault> {
    if d["Id"] != id || id.len() != 64 || !id.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(Fault);
    }
    // Observed host config must not carry host-shared escape hatches; otherwise the
    // observation is not of the reviewed guest and cannot count as a receipt.
    let hc = &d["HostConfig"];
    if hc["Privileged"] == true
        || ["NetworkMode", "PidMode", "IpcMode", "UTSMode", "UsernsMode"]
            .iter()
            .any(|k| hc[*k] == "host")
    {
        return Err(Fault);
    }
    if let Some(ms) = d["Mounts"].as_array() {
        for m in ms {
            let src = m["Source"].as_str().ok_or(Fault)?;
            let bad = src.ends_with("docker.sock")
                || src.contains(".X11-unix")
                || src == "/"
                || src == "/dev"
                || src.starts_with("/dev/input")
                || src.starts_with("/run/udev");
            if bad {
                return Err(Fault);
            }
        }
    }
    let state = &d["State"];
    let clean = state["Paused"] == false && state["Restarting"] == false && state["Dead"] == false;
    let status = match state["Status"].as_str() {
        Some("running")
            if clean
                && state["Running"] == true
                && state["Pid"].as_u64().is_some_and(|p| p > 0) =>
        {
            GuestState::Running
        }
        Some("exited") if clean && state["Running"] == false && state["Pid"] == 0 => {
            GuestState::Exited
        }
        _ => GuestState::Other,
    };
    Ok(Guest {
        id: id.into(),
        image: d["Image"].as_str().ok_or(Fault)?.into(),
        nonce: d["Config"]["Labels"]["mwow.nonce"]
            .as_str()
            .ok_or(Fault)?
            .into(),
        profile: d["Config"]["Labels"]["mwow.profile"]
            .as_str()
            .ok_or(Fault)?
            .into(),
        state: status,
    })
}
struct NativeIo {
    dir: PathBuf,
    uid: u32,
    prefix: (u64, u64),
    locks: Option<(File, File)>,
    child: Option<Child>,
    pidfd: Option<OwnedFd>,
    child_id: Option<ChildId>,
    socket: String,
    key: Key,
    listener: Option<UnixListener>,
    started: Instant,
}
impl Drop for NativeIo {
    fn drop(&mut self) {
        if let Some(child) = &mut self.child {
            while child.wait().is_err() {
                std::thread::sleep(Duration::from_millis(100));
            }
        }
        if self.listener.is_some() {
            let _ = fs::remove_file(self.dir.join("control.sock"));
        }
    }
}
impl Io for NativeIo {
    fn monotonic(&mut self) -> Result<Duration, Fault> {
        Ok(self.started.elapsed())
    }
    fn reserve(&mut self, key: &Key) -> Result<(), Fault> {
        let mut domain = exclusive(&self.dir.join("domain.lock"), self.uid)?;
        let prefix = exclusive(
            &self
                .dir
                .join(format!("prefix-{}-{}.lock", self.prefix.0, self.prefix.1)),
            self.uid,
        )?;
        let journal = self.dir.join("journal.json");
        let marker =
            private_file(&self.dir.join("domain.lock"), self.uid, 4096).map_err(|_| Fault)?;
        if journal.exists() {
            let b = private_file(&journal, self.uid, 8192).map_err(|_| Fault)?;
            let old: Journal = serde_json::from_slice(&b).map_err(|_| Fault)?;
            let prior: Key = serde_json::from_slice(&marker).map_err(|_| Fault)?;
            if old.key != prior || !matches!(old.phase, Phase::Stopped | Phase::Refused) {
                return Err(Fault);
            }
        } else if !marker.is_empty() {
            return Err(Fault);
        }
        // Durable epoch marker precedes first journal publication and ALL child effects.
        // If a journal write later fails, an older terminal journal cannot free this epoch.
        domain.set_len(0).map_err(|_| Fault)?;
        domain
            .write_all(&serde_json::to_vec(key).map_err(|_| Fault)?)
            .map_err(|_| Fault)?;
        domain.sync_all().map_err(|_| Fault)?;
        let control = self.dir.join("control.sock");
        if control.exists() {
            return Err(Fault);
        }
        let listener = UnixListener::bind(control).map_err(|_| Fault)?;
        listener.set_nonblocking(true).map_err(|_| Fault)?;
        self.listener = Some(listener);
        self.locks = Some((domain, prefix));
        self.key = key.clone();
        Ok(())
    }
    fn save(&mut self, j: &Journal) -> Result<(), Fault> {
        if self.locks.is_none() || j.key != self.key {
            return Err(Fault);
        }
        atomic_write(
            &self.dir,
            "journal.json",
            &serde_json::to_vec(j).map_err(|_| Fault)?,
        )
    }
    fn spawn(&mut self, p: &Prepared) -> Result<ChildId, SpawnError> {
        let config = labeled_config(p, &self.key).map_err(|_| SpawnError::NoChild)?;
        atomic_write(&self.dir, "active-config.toml", config.as_bytes())
            .map_err(|_| SpawnError::NoChild)?;
        let log = OpenOptions::new()
            .create(true)
            .append(true)
            .mode(0o600)
            .open(self.dir.join("wolf-private.log"))
            .map_err(|_| SpawnError::NoChild)?;
        let stderr = log.try_clone().map_err(|_| SpawnError::NoChild)?;
        let child = Command::new(&p.wolf)
            .env_clear()
            .current_dir(&p.home)
            .stdin(Stdio::null())
            .stdout(log)
            .stderr(stderr)
            .env("HOME", &p.home)
            .env("PATH", "/usr/bin:/bin")
            .env("XDG_RUNTIME_DIR", &p.runtime)
            .env("HOST_APPS_STATE_FOLDER", &p.state)
            .env("WOLF_CFG_FILE", self.dir.join("active-config.toml"))
            .env("WOLF_PRIVATE_KEY_FILE", &p.spec.private_key)
            .env("WOLF_PRIVATE_CERT_FILE", &p.spec.certificate)
            .env("WOLF_DOCKER_SOCKET", &p.spec.docker_socket)
            .env("WOLF_DOCKER_FAKE_UDEV_PATH", &p.spec.fake_udev.path)
            .env("WOLF_STOP_CONTAINER_ON_EXIT", "FALSE")
            .env("WOLF_ALLOW_PULSE_CONTAINER", "FALSE")
            .env("WOLF_ENABLE_MDNS", "FALSE")
            .env("WOLF_USE_ZERO_COPY", "FALSE")
            .env("WOLF_LOG_LEVEL", "INFO")
            .env("PULSE_SERVER", format!("{}/pulse-socket", p.runtime))
            .env("GST_PLUGIN_PATH", &p.plugins)
            .spawn()
            .map_err(|_| SpawnError::NoChild)?;
        let pid = child.id();
        self.child = Some(child);
        let id = ChildId {
            pid,
            start: process_start(pid).unwrap_or(0),
        };
        self.child_id = Some(id.clone());
        let fd = unsafe { pidfd_open(pid as i32, 0) };
        if fd < 0 {
            return Err(SpawnError::Uncertain(Some(id)));
        }
        self.pidfd = Some(unsafe { OwnedFd::from_raw_fd(fd) });
        if id.start == 0 {
            return Err(SpawnError::Uncertain(Some(id)));
        }
        Ok(id)
    }
    fn guests(&mut self, key: &Key) -> Result<Vec<Guest>, Fault> {
        let filters = json!({"label":[format!("mwow.nonce={}",key.nonce),format!("mwow.profile={}",key.profile)]});
        let list = get(
            &self.socket,
            &format!(
                "/v1.40/containers/json?all=1&filters={}",
                escape(&filters.to_string())
            ),
        )?;
        let list = list.as_array().ok_or(Fault)?;
        if list.len() > 1 {
            return Err(Fault);
        }
        let mut out = Vec::new();
        for g in list {
            let id = g["Id"].as_str().ok_or(Fault)?;
            if id.len() != 64 || !id.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err(Fault);
            }
            let detail = get(&self.socket, &format!("/v1.40/containers/{id}/json"))?;
            out.push(decode_inspection(&detail, id)?);
        }
        Ok(out)
    }
    fn child_wait(&mut self, id: &ChildId) -> Result<Option<i32>, Fault> {
        if self.child_id.as_ref() != Some(id) {
            return Err(Fault);
        }
        self.child
            .as_mut()
            .ok_or(Fault)?
            .try_wait()
            .map(|s| s.map(|s| s.code().unwrap_or(-1)))
            .map_err(|_| Fault)
    }
    fn graceful_stop(&mut self, id: &ChildId) -> Result<(), Fault> {
        if self.child_id.as_ref() != Some(id) {
            return Err(Fault);
        }
        if self
            .child
            .as_mut()
            .ok_or(Fault)?
            .try_wait()
            .map_err(|_| Fault)?
            .is_some()
        {
            return Ok(());
        }
        let fd = self.pidfd.as_ref().ok_or(Fault)?;
        if unsafe { pidfd_send_signal(fd.as_raw_fd(), 2, std::ptr::null(), 0) } != 0 {
            return Err(Fault);
        }
        Ok(())
    }
    fn release(&mut self, key: &Key) -> Result<(), Fault> {
        if key != &self.key {
            return Err(Fault);
        }
        self.locks.take();
        Ok(())
    }
}
#[derive(Serialize, Deserialize)]
struct Request {
    action: String,
    key: Key,
}
#[derive(Serialize, Deserialize)]
pub struct View {
    pub phase: Phase,
    pub completion: bool,
}
fn view(phase: Phase) -> View {
    View {
        phase,
        completion: phase == Phase::Stopped,
    }
}
fn check_key(j: &Journal, profile: &str, spec: &str) -> Result<(), String> {
    if j.key.profile != profile
        || j.key.spec != spec
        || j.key.boot != boot()?
        || process_start(j.key.pid)? != j.key.start
    {
        return Err(err());
    }
    Ok(())
}
pub fn command(action: &str, profile_path: &str, spec_path: &str) -> Result<View, String> {
    let mut source = NativeSource;
    if action != "start" {
        let pb = source.root_file(profile_path, crate::MAX_PROFILE)?;
        if pb.uid != 0 || pb.mode & 0o022 != 0 {
            return Err(err());
        }
        let p: Profile = crate::parse(&pb.bytes)?;
        if !crate::validate(&p).is_empty() || source.uid() != p.remote_uid {
            return Err(err());
        }
        let sb = source.root_file(spec_path, 32768)?;
        if sb.uid != 0 || sb.mode & 0o022 != 0 {
            return Err(err());
        }
        let dir = PathBuf::from(format!("{}/launcher", p.remote_home));
        source.private_dir(dir.to_str().unwrap(), p.remote_uid)?;
        let j: Journal = serde_json::from_slice(&private_file(
            &dir.join("journal.json"),
            p.remote_uid,
            8192,
        )?)
        .map_err(|_| err())?;
        if j.key.profile != digest(&pb.bytes) || j.key.spec != digest(&sb.bytes) {
            return Err(err());
        }
        let owner_alive =
            j.key.boot == boot()? && process_start(j.key.pid).ok() == Some(j.key.start);
        if !owner_alive {
            // Dead owner: report only the durable journal verdict. Never signal anything,
            // and never infer completion from owner absence alone.
            return Ok(view(match j.phase {
                Phase::Stopped | Phase::Refused if j.key.boot == boot()? => j.phase,
                _ => Phase::Unknown,
            }));
        }
        check_key(&j, &digest(&pb.bytes), &digest(&sb.bytes))?;
        let mut stream = UnixStream::connect(dir.join("control.sock")).map_err(|_| err())?;
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .map_err(|_| err())?;
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .map_err(|_| err())?;
        stream
            .write_all(
                &serde_json::to_vec(&Request {
                    action: action.into(),
                    key: j.key,
                })
                .map_err(|_| err())?,
            )
            .map_err(|_| err())?;
        stream
            .shutdown(std::net::Shutdown::Write)
            .map_err(|_| err())?;
        let mut b = Vec::new();
        stream.take(4097).read_to_end(&mut b).map_err(|_| err())?;
        if b.len() > 4096 {
            return Err(err());
        }
        return serde_json::from_slice(&b).map_err(|_| err());
    }
    let p = prepare(&mut source, profile_path, spec_path)?;
    let image = get(
        &p.spec.docker_socket,
        &format!("/v1.40/images/{}/json", escape(&p.image_id)),
    )
    .map_err(|_| err())?;
    if image["Id"] != p.image_id
        || !image["RepoDigests"]
            .as_array()
            .is_some_and(|a| a.iter().any(|d| d.as_str() == Some(p.image_ref.as_str())))
        || image["Config"]["Entrypoint"] != json!(["/opt/wolf/approved-entrypoint"])
    {
        return Err(err());
    }
    let key = Key {
        boot: boot()?,
        pid: std::process::id(),
        start: process_start(std::process::id())?,
        nonce: nonce()?,
        profile: p.profile_hash.clone(),
        spec: p.spec_hash.clone(),
    };
    let io = NativeIo {
        dir: p.lease.clone().into(),
        uid: p.uid,
        prefix: p.prefix_identity,
        locks: None,
        child: None,
        pidfd: None,
        child_id: None,
        socket: p.spec.docker_socket.clone(),
        key: key.clone(),
        listener: None,
        started: Instant::now(),
    };
    struct Signals(usize, usize);
    impl Drop for Signals {
        fn drop(&mut self) {
            unsafe {
                signal(2, self.0);
                signal(15, self.1);
            }
        }
    }
    STOP.store(false, Ordering::Relaxed);
    let _signals = unsafe {
        Signals(
            signal(2, stop_signal as *const () as usize),
            signal(15, stop_signal as *const () as usize),
        )
    };
    let mut c = Controller::start(io, &p, key).map_err(|_| err())?;
    if c.journal.phase == Phase::Refused {
        return Ok(view(Phase::Refused));
    }
    let mut stop_at = None;
    loop {
        if STOP.load(Ordering::Relaxed) && stop_at.is_none() {
            c.stop(&c.journal.key.clone());
            stop_at = Some(Instant::now());
        }
        if let Ok((mut stream, _)) = c.io.listener.as_ref().ok_or_else(err)?.accept() {
            let _ = stream.set_read_timeout(Some(Duration::from_millis(100)));
            let _ = stream.set_write_timeout(Some(Duration::from_millis(100)));
            let mut b = Vec::new();
            if (&mut stream).take(4097).read_to_end(&mut b).is_ok() && b.len() <= 4096 {
                if let Ok(req) = serde_json::from_slice::<Request>(&b) {
                    if req.key == c.journal.key && ["status", "stop"].contains(&req.action.as_str())
                    {
                        if req.action == "stop" {
                            c.stop(&req.key);
                            if stop_at.is_none() {
                                stop_at = Some(Instant::now());
                            }
                        }
                        let _ =
                            stream.write_all(&serde_json::to_vec(&view(c.journal.phase)).unwrap());
                    }
                }
            }
        }
        c.tick();
        let done = match &mut c.io.child {
            Some(child) => child.try_wait().ok().flatten().is_some(),
            None => true,
        };
        if done {
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    Ok(view(c.journal.phase))
}
