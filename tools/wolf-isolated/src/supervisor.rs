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
            net::UnixListener,
        },
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};
unsafe extern "C" {
    fn geteuid() -> u32;
    fn faccessat(dirfd: i32, path: *const std::ffi::c_char, mode: i32, flags: i32) -> i32;
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
    fn shared_render(&mut self, path: &str, gid: u32) -> Result<(), String> {
        let m = crate::metadata(path)?;
        if path != "/dev/dri/renderD128" || gid != 989 || unsafe { geteuid() } != 2001
            || !m.file_type().is_char_device() || m.uid() != 0 || m.gid() != gid
            || m.rdev() != ((226_u64 << 8) | 128)
            || !matches!(m.mode() & 0o7777, 0o660 | 0o666) {
            return Err(err());
        }
        if unsafe { faccessat(-100, c"/dev/dri/renderD128".as_ptr(), 6, 0x200) } != 0 {
            return Err(format!("errno={}", std::io::Error::last_os_error().raw_os_error().unwrap_or(-1)));
        }
        Ok(())
    }
    fn uinput_rw_access(&mut self) -> Result<(), String> {
        let m = crate::metadata("/dev/uinput")?;
        if unsafe { geteuid() } != 2001 || !m.file_type().is_char_device()
            || m.mode() & 0o007 != 0
            || unsafe { faccessat(-100, c"/dev/uinput".as_ptr(), 6, 0x200) } != 0 {
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
    fn gpu_nodes(&mut self, render: &str) -> Result<Vec<String>, String> {
        let name = Path::new(render)
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or_else(err)?;
        let mut found = Vec::new();
        let mut count = 0;
        for entry in fs::read_dir(format!("/sys/class/drm/{name}/device/drm")).map_err(|_| err())? {
            count += 1;
            if count > 32 {
                return Err(err());
            }
            let name = entry.map_err(|_| err())?.file_name();
            let name = name.to_str().ok_or_else(err)?;
            if ["card", "renderD"].iter().any(|p| {
                name.strip_prefix(p)
                    .is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
            }) {
                found.push(format!("/dev/dri/{name}"));
            }
        }
        found.sort();
        if found.len() != 2
            || !found.iter().any(|p| p.starts_with("/dev/dri/card"))
            || !found.iter().any(|p| p == render)
        {
            return Err(err());
        }
        Ok(found)
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
pub(crate) fn private_file(path: &Path, uid: u32, limit: u64) -> Result<Vec<u8>, String> {
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
pub(crate) fn exclusive(path: &Path, uid: u32) -> Result<File, Fault> {
    advisory_lock(path, uid, 2 | 4)
}
pub(crate) fn advisory_lock(path: &Path, uid: u32, operation: i32) -> Result<File, Fault> {
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
    if unsafe { flock(f.as_raw_fd(), operation) } != 0 {
        return Err(Fault);
    }
    Ok(f)
}
pub(crate) fn atomic_write(dir: &Path, name: &str, bytes: &[u8]) -> Result<(), Fault> {
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
fn get(socket: &str, path: &str) -> Result<Value, crate::request_io::Error> {
    get_with_limit(socket, path, Duration::from_millis(500))
}
fn get_with_limit(
    socket: &str,
    path: &str,
    limit: Duration,
) -> Result<Value, crate::request_io::Error> {
    use crate::request_io::{
        connect, read_response, write_request, Budget, Error, NativeClock, UnixConnector,
    };
    if !path.starts_with("/v1.40/")
        || path.len() > 4096
        || path.bytes().any(|b| b == b'\r' || b == b'\n')
    {
        return Err(Error::Fault);
    }
    let mut budget = Budget::new(NativeClock::new(|| STOP.load(Ordering::Relaxed)), limit)?;
    let mut connector = UnixConnector::new(Path::new(socket)).map_err(|_| Error::Fault)?;
    let mut stream = connect(&mut connector, &mut budget)?;
    let request = format!("GET {path} HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    write_request(&mut stream, &mut budget, request.as_bytes())?;
    // The response ends at Connection: close EOF; keep the HTTP request alive until then.
    let b = read_response(&mut stream, &mut budget, 256 * 1024)?;
    let split = b
        .windows(4)
        .position(|s| s == b"\r\n\r\n")
        .ok_or(Error::Fault)?;
    if split > 8192 {
        return Err(Error::Fault);
    }
    let header = std::str::from_utf8(&b[..split]).map_err(|_| Error::Fault)?;
    if !header
        .lines()
        .next()
        .is_some_and(|s| s.split_whitespace().nth(1) == Some("200"))
        || header.to_ascii_lowercase().contains("transfer-encoding:")
    {
        return Err(Error::Fault);
    }
    let lengths = header
        .lines()
        .filter_map(|l| l.split_once(':'))
        .filter(|(k, _)| k.eq_ignore_ascii_case("content-length"))
        .map(|(_, v)| v.trim().parse::<usize>())
        .collect::<Result<Vec<_>, _>>()
        .map_err(|_| Error::Fault)?;
    if lengths.len() > 1 || lengths.first().is_some_and(|n| *n != b.len() - split - 4) {
        return Err(Error::Fault);
    }
    let result = serde_json::from_slice(&b[split + 4..]).map_err(|_| Error::Fault)?;
    budget.remaining()?;
    Ok(result)
}
/// Decode the real Docker GET inspect path; uncertain/dead is never an exit receipt.
pub fn decode_inspection(
    d: &Value,
    id: &str,
    expected: &crate::inspection::Policy,
) -> Result<Guest, Fault> {
    if d["Id"] != id || id.len() != 64 || !id.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err(Fault);
    }
    expected.validate(d)?;
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
    inspection: crate::inspection::Policy,
    yielded_stop: bool,
    observation_deadline: Option<Duration>,
}
impl NativeIo {
    fn observation_get(&mut self, path: &str) -> Result<Value, Fault> {
        let mut limit = Duration::from_millis(500);
        if let Some(deadline) = self.observation_deadline {
            limit = limit.min(deadline.checked_sub(self.started.elapsed()).ok_or(Fault)?);
        }
        match get_with_limit(&self.socket, path, limit) {
            Ok(v) => Ok(v),
            Err(crate::request_io::Error::Interrupted) => {
                self.yielded_stop = true;
                Err(Fault)
            }
            Err(_) => Err(Fault),
        }
    }
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
    fn observation_deadline(&mut self, deadline: Option<Duration>) {
        self.observation_deadline = deadline;
    }
    fn take_stop_yield(&mut self) -> bool {
        std::mem::take(&mut self.yielded_stop)
    }
    fn monotonic(&mut self) -> Result<Duration, Fault> {
        Ok(self.started.elapsed())
    }
    fn reserve(&mut self, key: &Key) -> Result<(), Fault> {
        let store = crate::epoch::Store::new(&self.dir, self.uid);
        let domain = store.lease()?;
        let prefix = exclusive(
            &self
                .dir
                .join(format!("prefix-{}-{}.lock", self.prefix.0, self.prefix.1)),
            self.uid,
        )?;
        let listener = store.begin_control(key, || {
            let control = self.dir.join("control.sock");
            if control.exists() {
                return Err(Fault);
            }
            let listener = UnixListener::bind(control).map_err(|_| Fault)?;
            listener.set_nonblocking(true).map_err(|_| Fault)?;
            Ok(listener)
        })?;
        self.listener = Some(listener);
        self.locks = Some((domain, prefix));
        self.key = key.clone();
        Ok(())
    }
    fn save(&mut self, j: &Journal) -> Result<(), Fault> {
        if self.locks.is_none() || j.key != self.key {
            return Err(Fault);
        }
        crate::epoch::Store::new(&self.dir, self.uid).publish(j)
    }
    fn spawn(&mut self, p: &Prepared) -> Result<ChildId, SpawnError> {
        p.revalidate_shared_existing(&mut NativeSource)
            .map_err(|_| SpawnError::NoChild)?;
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
            .env("PULSE_CLIENTCONFIG", format!("{}/mwow-audio/configs/pulse-client.conf", p.runtime))
            .env("PULSE_COOKIE", format!("{}/mwow-audio/private-cookie", p.runtime))
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
        let list = self.observation_get(&format!(
            "/v1.40/containers/json?all=1&filters={}",
            escape(&filters.to_string())
        ))?;
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
            let detail = self.observation_get(&format!("/v1.40/containers/{id}/json"))?;
            out.push(decode_inspection(&detail, id, &self.inspection)?);
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
        let j = match crate::epoch::Store::new(&dir, p.remote_uid).snapshot() {
            Ok(j) => j,
            Err(_) => return Ok(view(Phase::Unknown)),
        };
        if j.key.profile != digest(&pb.bytes) || j.key.spec != digest(&sb.bytes) {
            return Err(err());
        }
        let owner_alive =
            j.key.boot == boot()? && process_start(j.key.pid).ok() == Some(j.key.start);
        if !owner_alive {
            // Dead owner: report only the durable journal verdict. Never signal anything,
            // and never infer completion from owner absence alone.
            return Ok(view(
                crate::epoch::Store::new(&dir, p.remote_uid).phase_when_dead(
                    &boot()?,
                    &digest(&pb.bytes),
                    &digest(&sb.bytes),
                ),
            ));
        }
        check_key(&j, &digest(&pb.bytes), &digest(&sb.bytes))?;
        if !crate::epoch::Store::new(&dir, p.remote_uid)
            .snapshot()
            .is_ok_and(|current| current.key == j.key)
        {
            return Ok(view(Phase::Unknown));
        }
        let expected_key = j.key.clone();
        let exchange = (|| -> Result<View, crate::request_io::Error> {
            use crate::request_io::*;
            let mut budget = Budget::new(NativeClock::new(|| false), Duration::from_secs(2))?;
            let mut connector =
                UnixConnector::new(&dir.join("control.sock")).map_err(|_| Error::Fault)?;
            let mut stream = connect(&mut connector, &mut budget)?;
            let bytes = serde_json::to_vec(&Request {
                action: action.into(),
                key: j.key,
            })
            .map_err(|_| Error::Fault)?;
            write_request(&mut stream, &mut budget, &bytes)?;
            stream
                .shutdown(std::net::Shutdown::Write)
                .map_err(|_| Error::Fault)?;
            let bytes = read_response(&mut stream, &mut budget, 4096)?;
            let reply: View = serde_json::from_slice(&bytes).map_err(|_| Error::Fault)?;
            budget.remaining()?;
            if reply.completion != (reply.phase == Phase::Stopped) {
                return Err(Error::Fault);
            }
            Ok(reply)
        })();
        if !crate::epoch::Store::new(&dir, p.remote_uid)
            .snapshot()
            .is_ok_and(|j| j.key == expected_key)
        {
            return Ok(view(Phase::Unknown));
        }
        return Ok(exchange.unwrap_or_else(|_| view(Phase::Unknown)));
    }
    let p = prepare(&mut source, profile_path, spec_path)?;
    let image = get(
        &p.spec.docker_socket,
        &format!("/v1.40/images/{}/json", escape(&p.image_id)),
    )
    .map_err(|_| "prelaunch stage=docker-image-get".to_owned())?;
    if image["Id"] != p.image_id
        || (p.image_kind == crate::ImageKind::RegistryDigest
            && !image["RepoDigests"]
                .as_array()
                .is_some_and(|a| a.iter().any(|d| d.as_str() == Some(p.image_ref.as_str()))))
        || image["Config"]["Entrypoint"] != json!(["/opt/wolf/approved-entrypoint"])
    {
        return Err("prelaunch stage=docker-image-identity".to_owned());
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
        inspection: p.inspection.clone(),
        yielded_stop: false,
        observation_deadline: None,
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
        if STOP.swap(false, Ordering::Relaxed) {
            c.stop(&c.journal.key.clone());
            stop_at = Some(Instant::now());
        }
        if let Ok((mut stream, _)) = c.io.listener.as_ref().ok_or_else(err)?.accept() {
            use crate::request_io::{read_response, write_request, Budget, NativeClock};
            if stream.set_nonblocking(true).is_ok() {
                let mut budget = Budget::new(
                    NativeClock::new(|| STOP.load(Ordering::Relaxed)),
                    Duration::from_millis(100),
                )
                .map_err(|_| err())?;
                if let Ok(bytes) = read_response(&mut stream, &mut budget, 4096) {
                    if let Ok(req) = serde_json::from_slice::<Request>(&bytes) {
                        if req.key == c.journal.key
                            && ["status", "stop"].contains(&req.action.as_str())
                        {
                            let current = crate::epoch::Store::new(&c.io.dir, c.io.uid)
                                .snapshot()
                                .is_ok_and(|j| j.key == c.journal.key);
                            let response = if current {
                                if req.action == "stop" {
                                    c.stop(&req.key);
                                    if stop_at.is_none() {
                                        stop_at = Some(Instant::now());
                                    }
                                }
                                view(c.journal.phase)
                            } else {
                                view(Phase::Unknown)
                            };
                            if let Ok(bytes) = serde_json::to_vec(&response) {
                                let _ = write_request(&mut stream, &mut budget, &bytes);
                            }
                        }
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
