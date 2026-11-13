//! Native journal/epoch storage, separate from domain-lifetime lease.
use crate::{
    runtime::*,
    supervisor::{atomic_write, exclusive, private_file},
};
use std::{
    fs::{File, OpenOptions},
    io::Write,
    os::unix::fs::OpenOptionsExt,
    path::{Path, PathBuf},
};
pub struct Store {
    dir: PathBuf,
    uid: u32,
}
impl Store {
    pub fn new(dir: &Path, uid: u32) -> Self {
        Self {
            dir: dir.into(),
            uid,
        }
    }
    pub fn lease(&self) -> Result<File, Fault> {
        exclusive(&self.dir.join("domain.lock"), self.uid)
    }
    pub fn snapshot(&self) -> Result<Journal, Fault> {
        // RED extraction: old status path read only the journal.
        serde_json::from_slice(
            &private_file(&self.dir.join("journal.json"), self.uid, 8192).map_err(|_| Fault)?,
        )
        .map_err(|_| Fault)
    }
    pub fn phase_when_dead(&self, boot: &str, profile: &str, spec: &str) -> Phase {
        match self.snapshot() {
            Ok(j)
                if j.key.boot == boot
                    && j.key.profile == profile
                    && j.key.spec == spec
                    && matches!(j.phase, Phase::Stopped | Phase::Refused) =>
            {
                j.phase
            }
            _ => Phase::Unknown,
        }
    }
    pub fn begin_control<T>(
        &self,
        key: &Key,
        bind: impl FnOnce() -> Result<T, Fault>,
    ) -> Result<T, Fault> {
        let marker =
            private_file(&self.dir.join("domain.lock"), self.uid, 4096).map_err(|_| Fault)?;
        if self.dir.join("journal.json").exists() {
            let old = self.snapshot()?;
            let prior: Key = serde_json::from_slice(&marker).map_err(|_| Fault)?;
            if old.key != prior || !matches!(old.phase, Phase::Stopped | Phase::Refused) {
                return Err(Fault);
            }
        } else if !marker.is_empty() {
            return Err(Fault);
        }
        let mut f = OpenOptions::new()
            .write(true)
            .truncate(true)
            .mode(0o600)
            .open(self.dir.join("domain.lock"))
            .map_err(|_| Fault)?;
        f.write_all(&serde_json::to_vec(key).map_err(|_| Fault)?)
            .map_err(|_| Fault)?;
        f.sync_all().map_err(|_| Fault)?;
        bind()
    }
    pub fn publish(&self, j: &Journal) -> Result<(), Fault> {
        atomic_write(
            &self.dir,
            "journal.json",
            &serde_json::to_vec(j).map_err(|_| Fault)?,
        )
    }
}
