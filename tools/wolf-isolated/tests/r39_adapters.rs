use std::{
    cell::Cell,
    fs,
    io::{self, Read, Write},
    os::unix::fs::{MetadataExt, PermissionsExt},
    path::PathBuf,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};
use wolf_isolated::{
    epoch::Store,
    request_io::{self, Budget, Clock, Error, Interest, Stream},
    runtime::*,
};
static NEXT: AtomicU64 = AtomicU64::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let p = PathBuf::from(std::env::var_os("CARGO_TARGET_DIR").unwrap()).join(format!(
            "r39-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&p).unwrap();
        fs::set_permissions(&p, fs::Permissions::from_mode(0o700)).unwrap();
        Self(p)
    }
    fn store(&self) -> Store {
        Store::new(&self.0, fs::metadata(&self.0).unwrap().uid())
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
fn key(n: &str) -> Key {
    Key {
        boot: "12345678-1234-1234-1234-123456789012".into(),
        pid: 900,
        start: 12,
        nonce: n.repeat(64),
        profile: "p".repeat(64),
        spec: "s".repeat(64),
    }
}
fn terminal(s: &Store, k: &Key, phase: Phase) {
    let _lease = s.lease().unwrap();
    s.begin_control(k, || Ok(())).unwrap();
    s.publish(&Journal {
        key: k.clone(),
        phase,
        child: None,
        cid: None,
    })
    .unwrap();
}
#[test]
fn r39_epoch_bind_failure_does_not_resurrect_terminal_a() {
    for phase in [Phase::Stopped, Phase::Refused] {
        let t = Temp::new();
        let s = t.store();
        let a = key("a");
        terminal(&s, &a, phase);
        assert_eq!(s.phase_when_dead(&a.boot, &a.profile, &a.spec), phase);
        let _lease = s.lease().unwrap();
        let b = key("b");
        assert!(s.begin_control(&b, || Err::<(), _>(Fault)).is_err());
        assert_eq!(
            s.phase_when_dead(&a.boot, &a.profile, &a.spec),
            Phase::Unknown
        );
    }
}
#[test]
fn r39_epoch_actual_atomic_save_failure_and_missing_marker_are_unknown() {
    let t = Temp::new();
    let s = t.store();
    let a = key("a");
    terminal(&s, &a, Phase::Stopped);
    let _lease = s.lease().unwrap();
    let b = key("b");
    s.begin_control(&b, || Ok(())).unwrap();
    fs::write(t.0.join(".journal.json.tmp"), b"blocked temporary").unwrap();
    assert!(s
        .publish(&Journal {
            key: b,
            phase: Phase::Unknown,
            child: None,
            cid: None
        })
        .is_err());
    assert_eq!(
        s.phase_when_dead(&a.boot, &a.profile, &a.spec),
        Phase::Unknown
    );
    fs::remove_file(t.0.join("domain.lock")).unwrap();
    assert!(s.snapshot().is_err());
}
#[derive(Clone)]
struct Time {
    now: Rc<Cell<u64>>,
    cancel: u64,
}
impl Clock for Time {
    fn now(&mut self) -> Result<Duration, Error> {
        Ok(Duration::from_millis(self.now.get()))
    }
    fn cancelled(&self) -> bool {
        self.now.get() >= self.cancel
    }
}
struct Drip {
    time: Rc<Cell<u64>>,
    left: usize,
    step: u64,
    backward: bool,
}
impl Read for Drip {
    fn read(&mut self, b: &mut [u8]) -> io::Result<usize> {
        if self.left == 0 {
            return Ok(0);
        }
        let n = if self.backward && self.left == 19 {
            50
        } else {
            self.time.get() + self.step
        };
        self.time.set(n);
        b[0] = b'x';
        self.left -= 1;
        Ok(1)
    }
}
impl Write for Drip {
    fn write(&mut self, b: &[u8]) -> io::Result<usize> {
        Ok(b.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
impl Stream for Drip {
    fn wait(&mut self, _: Interest, remaining: Duration) -> io::Result<()> {
        self.time
            .set(self.time.get() + remaining.as_millis().min(10) as u64);
        Ok(())
    }
}
#[test]
fn r39_actual_response_drip_cannot_reset_whole_request_budget() {
    let now = Rc::new(Cell::new(0));
    let mut budget = Budget::new(
        Time {
            now: now.clone(),
            cancel: u64::MAX,
        },
        Duration::from_millis(500),
    )
    .unwrap();
    let mut drip = Drip {
        time: now,
        left: 20,
        step: 80,
        backward: false,
    };
    assert_eq!(
        request_io::read_response(&mut drip, &mut budget, 4096),
        Err(Error::Timeout)
    );
}
#[test]
fn r39_response_progress_honors_stop_and_backward_clock() {
    for backward in [false, true] {
        let now = Rc::new(Cell::new(0));
        let mut budget = Budget::new(
            Time {
                now: now.clone(),
                cancel: if backward { u64::MAX } else { 160 },
            },
            Duration::from_millis(500),
        )
        .unwrap();
        let mut drip = Drip {
            time: now,
            left: 20,
            step: 100,
            backward,
        };
        assert_eq!(
            request_io::read_response(&mut drip, &mut budget, 4096),
            Err(if backward {
                Error::Fault
            } else {
                Error::Interrupted
            })
        );
    }
}

struct Pending {
    time: Rc<Cell<u64>>,
    calls: usize,
    full: bool,
}
impl request_io::Connector for Pending {
    type Connection = Drip;
    fn step(&mut self) -> io::Result<Option<Drip>> {
        self.calls += 1;
        if self.full {
            Err(io::Error::from(io::ErrorKind::WouldBlock))
        } else {
            Ok(None)
        }
    }
    fn wait(&mut self, left: Duration) -> io::Result<()> {
        self.time.set(self.time.get() + left.as_millis() as u64);
        Ok(())
    }
}
#[test]
fn bounded_connect_pending_and_full_backlog_do_not_retry_globally() {
    let now = Rc::new(Cell::new(0));
    let mut budget = Budget::new(
        Time {
            now: now.clone(),
            cancel: u64::MAX,
        },
        Duration::from_millis(50),
    )
    .unwrap();
    let mut connector = Pending {
        time: now.clone(),
        calls: 0,
        full: false,
    };
    assert!(matches!(
        request_io::connect(&mut connector, &mut budget),
        Err(Error::Timeout)
    ));
    assert!(connector.calls <= 5);
    assert_eq!(now.get(), 50);
    let mut budget = Budget::new(
        Time {
            now: now.clone(),
            cancel: u64::MAX,
        },
        Duration::from_millis(50),
    )
    .unwrap();
    let mut connector = Pending {
        time: now,
        calls: 0,
        full: true,
    };
    assert!(matches!(
        request_io::connect(&mut connector, &mut budget),
        Err(Error::Fault)
    ));
    assert_eq!(connector.calls, 1);
}
struct Writer {
    time: Rc<Cell<u64>>,
}
impl Read for Writer {
    fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
        Ok(0)
    }
}
impl Write for Writer {
    fn write(&mut self, _: &[u8]) -> io::Result<usize> {
        self.time.set(self.time.get() + 80);
        Ok(1)
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}
impl Stream for Writer {
    fn wait(&mut self, _: Interest, _: Duration) -> io::Result<()> {
        Ok(())
    }
}
#[test]
fn write_and_read_share_the_original_absolute_budget() {
    let now = Rc::new(Cell::new(0));
    let mut budget = Budget::new(
        Time {
            now: now.clone(),
            cancel: u64::MAX,
        },
        Duration::from_millis(250),
    )
    .unwrap();
    let mut writer = Writer { time: now.clone() };
    assert_eq!(
        request_io::write_request(&mut writer, &mut budget, b"123456"),
        Err(Error::Timeout)
    );
    let mut budget = Budget::new(
        Time {
            now: now.clone(),
            cancel: u64::MAX,
        },
        Duration::from_millis(250),
    )
    .unwrap();
    request_io::write_request(&mut writer, &mut budget, b"12").unwrap();
    let mut drip = Drip {
        time: now,
        left: 4,
        step: 80,
        backward: false,
    };
    assert_eq!(
        request_io::read_response(&mut drip, &mut budget, 100),
        Err(Error::Timeout)
    );
}
#[test]
fn actual_nonblocking_socketpair_timeout_eof_and_reply() {
    use std::os::unix::net::UnixStream;
    let (mut left, _peer) = UnixStream::pair().unwrap();
    left.set_nonblocking(true).unwrap();
    let mut budget = Budget::new(
        request_io::NativeClock::new(|| false),
        Duration::from_millis(20),
    )
    .unwrap();
    assert_eq!(
        request_io::read_response(&mut left, &mut budget, 100),
        Err(Error::Timeout)
    );
    let (mut left, mut right) = UnixStream::pair().unwrap();
    left.set_nonblocking(true).unwrap();
    right.set_nonblocking(true).unwrap();
    right.write_all(b"frame").unwrap();
    right.shutdown(std::net::Shutdown::Write).unwrap();
    let mut budget = Budget::new(
        request_io::NativeClock::new(|| false),
        Duration::from_secs(1),
    )
    .unwrap();
    assert_eq!(
        request_io::read_response(&mut left, &mut budget, 100).unwrap(),
        b"frame"
    );
    request_io::write_request(&mut left, &mut budget, b"reply").unwrap();
    let mut bytes = [0; 5];
    right.read_exact(&mut bytes).unwrap();
    assert_eq!(&bytes, b"reply");
}
#[test]
fn partial_epoch_and_stale_publish_never_ack_current_terminal() {
    let t = Temp::new();
    let s = t.store();
    let a = key("a");
    terminal(&s, &a, Phase::Stopped);
    let _lease = s.lease().unwrap();
    let b = key("b");
    s.begin_control(&b, || Ok(())).unwrap();
    assert!(s
        .publish(&Journal {
            key: a.clone(),
            phase: Phase::Stopped,
            child: None,
            cid: None
        })
        .is_err());
    assert_eq!(
        s.phase_when_dead(&a.boot, &a.profile, &a.spec),
        Phase::Unknown
    );
    fs::write(t.0.join("domain.lock"), b"{").unwrap();
    assert_eq!(
        s.phase_when_dead(&a.boot, &a.profile, &a.spec),
        Phase::Unknown
    );
}
#[test]
fn epoch_writer_busy_is_nonblocking_unknown_and_bind_runs_after_gate_release() {
    use std::{
        fs::OpenOptions,
        os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    };
    unsafe extern "C" {
        fn flock(fd: i32, operation: i32) -> i32;
    }
    let t = Temp::new();
    let s = t.store();
    let a = key("a");
    let _lease = s.lease().unwrap();
    s.begin_control(&a, || {
        s.publish(&Journal {
            key: a.clone(),
            phase: Phase::Stopped,
            child: None,
            cid: None,
        })?;
        Ok(())
    })
    .unwrap();
    let f = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .open(t.0.join("epoch.lock"))
        .unwrap();
    assert_eq!(unsafe { flock(f.as_raw_fd(), 2 | 4) }, 0);
    assert!(s.snapshot().is_err());
    drop(f);
    assert_eq!(
        s.phase_when_dead(&a.boot, &a.profile, &a.spec),
        Phase::Stopped
    );
}
