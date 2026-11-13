//! Coherent native epoch/journal publication. Short file gate never spans bind/socket/callback work.
use crate::{
    runtime::*,
    supervisor::{advisory_lock, atomic_write, exclusive, private_file},
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
    fn journal(&self) -> Result<Journal, Fault> {
        serde_json::from_slice(
            &private_file(&self.dir.join("journal.json"), self.uid, 8192).map_err(|_| Fault)?,
        )
        .map_err(|_| Fault)
    }
    fn marker(&self) -> Result<Key, Fault> {
        serde_json::from_slice(
            &private_file(&self.dir.join("domain.lock"), self.uid, 4096).map_err(|_| Fault)?,
        )
        .map_err(|_| Fault)
    }
    pub fn snapshot(&self) -> Result<Journal, Fault> {
        let _epoch = advisory_lock(&self.dir.join("epoch.lock"), self.uid, 1 | 4)?;
        let marker = self.marker()?;
        let j = self.journal()?;
        if marker != j.key {
            return Err(Fault);
        }
        Ok(j)
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
    fn begin(&self, key: &Key) -> Result<(), Fault> {
        let _epoch = advisory_lock(&self.dir.join("epoch.lock"), self.uid, 2 | 4)?;
        let bytes =
            private_file(&self.dir.join("domain.lock"), self.uid, 4096).map_err(|_| Fault)?;
        if self.dir.join("journal.json").exists() {
            let old = self.journal()?;
            let prior: Key = serde_json::from_slice(&bytes).map_err(|_| Fault)?;
            if old.key != prior || !matches!(old.phase, Phase::Stopped | Phase::Refused) {
                return Err(Fault);
            }
        } else if !bytes.is_empty() {
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
        f.sync_all().map_err(|_| Fault)
    }
    pub fn begin_control<T>(
        &self,
        key: &Key,
        bind: impl FnOnce() -> Result<T, Fault>,
    ) -> Result<T, Fault> {
        self.begin(key)?;
        bind()
    }
    pub fn publish(&self, j: &Journal) -> Result<(), Fault> {
        let _epoch = advisory_lock(&self.dir.join("epoch.lock"), self.uid, 2 | 4)?;
        if self.marker()? != j.key {
            return Err(Fault);
        }
        atomic_write(
            &self.dir,
            "journal.json",
            &serde_json::to_vec(j).map_err(|_| Fault)?,
        )
    }
}
