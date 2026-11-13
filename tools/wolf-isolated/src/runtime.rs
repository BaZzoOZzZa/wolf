//! Actual outer-controller. IO has no guest mutation method. No shared locks or worker threads.
use crate::runtime_spec::Prepared;
use serde::{Deserialize, Serialize};
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct Key {
    pub boot: String,
    pub pid: u32,
    pub start: u64,
    pub nonce: String,
    pub profile: String,
    pub spec: String,
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct ChildId {
    pub pid: u32,
    pub start: u64,
}
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum Phase {
    AwaitingGuest,
    Running,
    GuestExited,
    Stopping,
    Unknown,
    Stopped,
    Refused,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Journal {
    pub key: Key,
    pub phase: Phase,
    pub child: Option<ChildId>,
    pub cid: Option<String>,
}
#[derive(Clone, Debug)]
pub struct Guest {
    pub id: String,
    pub image: String,
    pub nonce: String,
    pub profile: String,
    pub state: GuestState,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GuestState {
    Running,
    Exited,
    Other,
}
#[derive(Debug)]
pub enum SpawnError {
    NoChild,
    Uncertain(Option<ChildId>),
}
#[derive(Debug)]
pub struct Fault;
pub trait Io {
    fn observation_deadline(&mut self, _deadline: Option<std::time::Duration>) {}
    fn take_stop_yield(&mut self) -> bool {
        false
    }
    fn monotonic(&mut self) -> Result<std::time::Duration, Fault>;
    fn reserve(&mut self, key: &Key) -> Result<(), Fault>;
    fn save(&mut self, journal: &Journal) -> Result<(), Fault>;
    fn spawn(&mut self, prepared: &Prepared) -> Result<ChildId, SpawnError>;
    fn guests(&mut self, key: &Key) -> Result<Vec<Guest>, Fault>; // read-only list, all=true
    fn child_wait(&mut self, child: &ChildId) -> Result<Option<i32>, Fault>; // Some means actual wait/reap
    fn graceful_stop(&mut self, child: &ChildId) -> Result<(), Fault>; // retained exact child/pidfd only
    fn release(&mut self, key: &Key) -> Result<(), Fault>;
}
pub struct Controller<I: Io> {
    pub io: I,
    pub journal: Journal,
    image: String,
    seen_running: bool,
    stop_started: Option<std::time::Duration>,
}
impl<I: Io> Controller<I> {
    pub fn start(mut io: I, prepared: &Prepared, key: Key) -> Result<Self, I> {
        if key.profile != prepared.profile_hash
            || key.spec != prepared.spec_hash
            || key.pid == 0
            || key.start == 0
            || key.boot.len() != 36
            || !crate::hex(&key.nonce)
        {
            return Err(io);
        }
        if io.reserve(&key).is_err() {
            return Err(io);
        }
        let mut c = Self {
            io,
            journal: Journal {
                key,
                phase: Phase::AwaitingGuest,
                child: None,
                cid: None,
            },
            image: prepared.image_id.clone(),
            seen_running: false,
            stop_started: None,
        };
        if c.io.save(&c.journal).is_err() {
            c.unknown();
            return Ok(c);
        }
        match c.io.spawn(prepared) {
            Ok(child) => {
                c.journal.child = Some(child);
                if c.io.save(&c.journal).is_err() {
                    c.unknown();
                }
            }
            Err(SpawnError::NoChild) => {
                c.journal.phase = Phase::Refused;
                if c.io.save(&c.journal).is_err() || c.io.release(&c.journal.key).is_err() {
                    c.unknown();
                }
            }
            Err(SpawnError::Uncertain(child)) => {
                c.journal.child = child;
                c.unknown();
            }
        }
        Ok(c)
    }
    fn unknown(&mut self) {
        self.journal.phase = Phase::Unknown;
        let _ = self.io.save(&self.journal);
    }
    pub fn stop(&mut self, expected: &Key) -> bool {
        if expected != &self.journal.key {
            return false;
        }
        if matches!(self.journal.phase, Phase::Stopped | Phase::Refused) {
            return true;
        }
        if self.stop_started.is_none() {
            match self.io.monotonic() {
                Ok(now) => self.stop_started = Some(now),
                Err(_) => self.unknown(),
            }
        }
        if self.journal.phase != Phase::Unknown {
            self.journal.phase = Phase::Stopping;
        }
        if self.io.save(&self.journal).is_err() {
            self.unknown();
        }
        if let Some(child) = self.journal.child.clone() {
            if self.io.graceful_stop(&child).is_err() {
                self.unknown();
            }
        } else {
            self.unknown();
        }
        true
    }
    fn poll_deadline(&mut self) {
        if let Some(start) = self.stop_started {
            match self.io.monotonic() {
                Ok(now)
                    if now
                        .checked_sub(start)
                        .is_some_and(|elapsed| elapsed < std::time::Duration::from_secs(5)) => {}
                _ => self.unknown(),
            }
        }
    }
    pub fn tick(&mut self) -> Phase {
        if matches!(self.journal.phase, Phase::Stopped | Phase::Refused) {
            return self.journal.phase;
        }
        self.poll_deadline();
        self.io.observation_deadline(
            self.stop_started
                .and_then(|s| s.checked_add(std::time::Duration::from_secs(5))),
        );
        let guests = match self.io.guests(&self.journal.key) {
            Ok(v) if v.len() <= 1 => v,
            Err(_) if self.io.take_stop_yield() => {
                self.stop(&self.journal.key.clone());
                self.poll_deadline();
                return self.journal.phase;
            }
            _ => {
                self.unknown();
                return self.journal.phase;
            }
        };
        let mut exited = false;
        if let Some(g) = guests.first() {
            if g.id.len() != 64
                || !g.id.bytes().all(|b| b.is_ascii_hexdigit())
                || g.nonce != self.journal.key.nonce
                || g.profile != self.journal.key.profile
                || g.image != self.image
                || self.journal.cid.as_ref().is_some_and(|id| id != &g.id)
            {
                self.unknown();
                return self.journal.phase;
            }
            self.journal.cid = Some(g.id.clone());
            if g.state == GuestState::Running {
                self.seen_running = true;
                if self.journal.phase == Phase::AwaitingGuest {
                    self.journal.phase = Phase::Running;
                }
            }
            exited = g.state == GuestState::Exited;
        } else if self.journal.cid.is_some() {
            self.unknown();
        }
        let child_exited = match self.journal.child.clone() {
            Some(child) => match self.io.child_wait(&child) {
                Ok(code) => code.is_some(),
                Err(_) => {
                    self.unknown();
                    false
                }
            },
            None => {
                self.unknown();
                false
            }
        };
        self.poll_deadline();
        if exited && self.seen_running && !child_exited && self.journal.phase != Phase::Unknown {
            self.journal.phase = Phase::GuestExited;
        }
        if child_exited {
            if exited && self.seen_running && self.journal.phase != Phase::Unknown {
                self.journal.phase = Phase::Stopped;
                if self.io.save(&self.journal).is_err()
                    || self.io.release(&self.journal.key).is_err()
                {
                    self.unknown();
                }
            } else {
                self.unknown();
            }
        } else if self.io.save(&self.journal).is_err() {
            self.unknown();
        }
        self.journal.phase
    }
    pub fn deadline(&mut self) {
        if !matches!(self.journal.phase, Phase::Stopped | Phase::Refused) {
            self.unknown();
        }
    }
}
