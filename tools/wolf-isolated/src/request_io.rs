//! One absolute monotonic budget across connect, write and the ENTIRE response.
//! No per-byte deadline reset, worker, socket listener, daemon or mutation protocol.
use std::{
    io::{self, Read, Write},
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::{ffi::OsStrExt, net::UnixStream},
    },
    path::Path,
    time::{Duration, Instant},
};
#[derive(Debug, PartialEq, Eq)]
pub enum Error {
    Fault,
    Timeout,
    Interrupted,
}
pub trait Clock {
    fn now(&mut self) -> Result<Duration, Error>;
    fn cancelled(&self) -> bool {
        false
    }
}
pub struct NativeClock {
    start: Instant,
    cancel: fn() -> bool,
}
impl NativeClock {
    pub fn new(cancel: fn() -> bool) -> Self {
        Self {
            start: Instant::now(),
            cancel,
        }
    }
}
impl Clock for NativeClock {
    fn now(&mut self) -> Result<Duration, Error> {
        Ok(self.start.elapsed())
    }
    fn cancelled(&self) -> bool {
        (self.cancel)()
    }
}
pub struct Budget<C: Clock> {
    clock: C,
    end: Duration,
    last: Duration,
}
impl<C: Clock> Budget<C> {
    pub fn new(mut clock: C, limit: Duration) -> Result<Self, Error> {
        let now = clock.now()?;
        Ok(Self {
            end: now.checked_add(limit).ok_or(Error::Fault)?,
            last: now,
            clock,
        })
    }
    pub fn remaining(&mut self) -> Result<Duration, Error> {
        if self.clock.cancelled() {
            return Err(Error::Interrupted);
        }
        let now = self.clock.now()?;
        if now < self.last {
            return Err(Error::Fault);
        }
        self.last = now;
        self.end
            .checked_sub(now)
            .filter(|d| !d.is_zero())
            .ok_or(Error::Timeout)
    }
}
#[derive(Clone, Copy)]
pub enum Interest {
    Read,
    Write,
}
pub trait Stream: Read + Write {
    fn wait(&mut self, interest: Interest, remaining: Duration) -> io::Result<()>;
}
fn wait<S: Stream, C: Clock>(
    s: &mut S,
    b: &mut Budget<C>,
    interest: Interest,
) -> Result<(), Error> {
    let remaining = b.remaining()?;
    match s.wait(interest, remaining.min(Duration::from_millis(10))) {
        Ok(()) => (),
        Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
        Err(_) => return Err(Error::Fault),
    }
    b.remaining()?;
    Ok(())
}
pub fn read_response<S: Stream, C: Clock>(
    s: &mut S,
    b: &mut Budget<C>,
    cap: usize,
) -> Result<Vec<u8>, Error> {
    let mut out = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        b.remaining()?;
        let n = match s.read(&mut chunk) {
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                wait(s, b, Interest::Read)?;
                continue;
            }
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => return Err(Error::Fault),
        };
        b.remaining()?;
        if n == 0 {
            return Ok(out);
        }
        if n > cap.saturating_sub(out.len()) {
            return Err(Error::Fault);
        }
        out.extend_from_slice(&chunk[..n]);
    }
}
pub fn write_request<S: Stream, C: Clock>(
    s: &mut S,
    b: &mut Budget<C>,
    bytes: &[u8],
) -> Result<(), Error> {
    let mut sent = 0;
    while sent < bytes.len() {
        b.remaining()?;
        match s.write(&bytes[sent..]) {
            Ok(0) => return Err(Error::Fault),
            Ok(n) => sent += n,
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => wait(s, b, Interest::Write)?,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
            Err(_) => return Err(Error::Fault),
        }
    }
    b.remaining()?;
    Ok(())
}
pub trait Connector {
    type Connection: Stream;
    fn step(&mut self) -> io::Result<Option<Self::Connection>>;
    fn wait(&mut self, remaining: Duration) -> io::Result<()>;
}
pub fn connect<C: Connector, T: Clock>(
    c: &mut C,
    b: &mut Budget<T>,
) -> Result<C::Connection, Error> {
    loop {
        b.remaining()?;
        let ready = c.step().map_err(|_| Error::Fault)?;
        b.remaining()?;
        if let Some(stream) = ready {
            return Ok(stream);
        }
        let remaining = b.remaining()?.min(Duration::from_millis(10));
        match c.wait(remaining) {
            Ok(()) => (),
            Err(e) if e.kind() == io::ErrorKind::Interrupted => (),
            Err(_) => return Err(Error::Fault),
        }
    }
}
#[repr(C)]
struct Address {
    family: u16,
    path: [u8; 108],
}
#[repr(C)]
struct PollFd {
    fd: i32,
    events: i16,
    revents: i16,
}
unsafe extern "C" {
    fn socket(domain: i32, kind: i32, protocol: i32) -> i32;
    #[link_name = "connect"]
    fn sys_connect(fd: i32, address: *const Address, len: u32) -> i32;
    fn poll(fds: *mut PollFd, n: usize, timeout: i32) -> i32;
    fn getsockopt(fd: i32, level: i32, name: i32, value: *mut i32, len: *mut u32) -> i32;
    fn getpeername(fd: i32, address: *mut Address, len: *mut u32) -> i32;
}
fn poll_fd(fd: i32, interest: Interest, duration: Duration) -> io::Result<()> {
    let mut p = PollFd {
        fd,
        events: match interest {
            Interest::Read => 1,
            Interest::Write => 4,
        },
        revents: 0,
    };
    let ms = duration.as_millis().clamp(1, 10) as i32;
    let rc = unsafe { poll(&mut p, 1, ms) };
    if rc < 0 {
        return Err(io::Error::last_os_error());
    }
    if p.revents & 32 != 0 {
        return Err(io::Error::from(io::ErrorKind::InvalidInput));
    }
    Ok(())
}
impl Stream for UnixStream {
    fn wait(&mut self, interest: Interest, remaining: Duration) -> io::Result<()> {
        poll_fd(self.as_raw_fd(), interest, remaining)
    }
}
pub struct UnixConnector {
    stream: Option<UnixStream>,
    address: Address,
    len: u32,
    started: bool,
}
impl UnixConnector {
    pub fn new(path: &Path) -> io::Result<Self> {
        let bytes = path.as_os_str().as_bytes();
        if bytes.is_empty() || bytes.len() >= 108 || bytes.contains(&0) {
            return Err(io::Error::from(io::ErrorKind::InvalidInput));
        }
        let fd = unsafe { socket(1, 1 | 0x800 | 0x80000, 0) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // Transfer the newly owned fd immediately, before any fallible work.
        let stream = unsafe { UnixStream::from_raw_fd(fd) };
        let mut address = Address {
            family: 1,
            path: [0; 108],
        };
        address.path[..bytes.len()].copy_from_slice(bytes);
        Ok(Self {
            stream: Some(stream),
            address,
            len: (2 + bytes.len() + 1) as u32,
            started: false,
        })
    }
}
impl Connector for UnixConnector {
    type Connection = UnixStream;
    fn step(&mut self) -> io::Result<Option<UnixStream>> {
        let fd = self
            .stream
            .as_ref()
            .ok_or_else(|| io::Error::from(io::ErrorKind::NotConnected))?
            .as_raw_fd();
        if !self.started {
            self.started = true;
            let rc = unsafe { sys_connect(fd, &self.address, self.len) };
            if rc == 0 {
                return Ok(self.stream.take());
            }
            let e = io::Error::last_os_error();
            if e.raw_os_error() != Some(115) {
                return Err(e);
            }
        } else {
            let mut error = 0i32;
            let mut len = std::mem::size_of::<i32>() as u32;
            if unsafe { getsockopt(fd, 1, 4, &mut error, &mut len) } != 0 {
                return Err(io::Error::last_os_error());
            }
            if error != 0 {
                return Err(io::Error::from_raw_os_error(error));
            }
        }
        let mut peer = Address {
            family: 0,
            path: [0; 108],
        };
        let mut len = std::mem::size_of::<Address>() as u32;
        if unsafe { getpeername(fd, &mut peer, &mut len) } == 0 {
            return Ok(self.stream.take());
        }
        let e = io::Error::last_os_error();
        if e.kind() != io::ErrorKind::NotConnected {
            return Err(e);
        }
        Ok(None)
    }
    fn wait(&mut self, remaining: Duration) -> io::Result<()> {
        poll_fd(
            self.stream
                .as_ref()
                .ok_or_else(|| io::Error::from(io::ErrorKind::NotConnected))?
                .as_raw_fd(),
            Interest::Write,
            remaining,
        )
    }
}
