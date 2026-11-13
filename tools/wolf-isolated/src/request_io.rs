//! Whole-request budget and primitive nonblocking stream seam shared by all socket users.
use std::{
    io::{self, Read, Write},
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
pub fn read_response<S: Read, C: Clock>(
    stream: &mut S,
    _budget: &mut Budget<C>,
    cap: usize,
) -> Result<Vec<u8>, Error> {
    // RED extraction: legacy read_to_end bounded bytes, not total elapsed time.
    let mut bytes = Vec::new();
    stream
        .take(cap as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| Error::Fault)?;
    if bytes.len() > cap {
        return Err(Error::Fault);
    }
    Ok(bytes)
}
