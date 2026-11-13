//! Retained data-only expectation from root-prepared config and native metadata, never inspect output.
#[derive(Clone)]
pub struct Policy {
    pub(crate) uid: u32,
    pub(crate) image: String,
    pub(crate) runtime: String,
    pub(crate) state: String,
    pub(crate) prefix: String,
    pub(crate) game: String,
    pub(crate) helper: String,
    pub(crate) gpu: Vec<String>,
    pub(crate) rules: Vec<String>,
}
impl Policy {
    pub(crate) fn new(
        p: &crate::Profile,
        state: &str,
        helper: &str,
        gpu: Vec<String>,
        rules: Vec<String>,
        image: &str,
    ) -> Self {
        Self {
            uid: p.remote_uid,
            image: image.into(),
            runtime: p.runtime.clone(),
            state: state.into(),
            prefix: p.prefix.working.clone(),
            game: p.game_data.clone(),
            helper: helper.into(),
            gpu,
            rules,
        }
    }
}

use crate::runtime::Fault;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
fn strings(v: &Value) -> Result<BTreeSet<String>, Fault> {
    let array = v.as_array().ok_or(Fault)?;
    let mut set = BTreeSet::new();
    for v in array {
        if !set.insert(v.as_str().ok_or(Fault)?.to_owned()) {
            return Err(Fault);
        }
    }
    Ok(set)
}
fn caps(v: &Value) -> Result<BTreeSet<String>, Fault> {
    let set = strings(v)?;
    Ok(set
        .into_iter()
        .map(|s| s.strip_prefix("CAP_").unwrap_or(&s).to_owned())
        .collect())
}
fn permission(v: &Value) -> Result<(), Fault> {
    let p = v.as_str().ok_or(Fault)?;
    if p.len() != 3 || !b"rwm".iter().all(|b| p.as_bytes().contains(b)) {
        return Err(Fault);
    }
    Ok(())
}
impl Policy {
    pub fn validate(&self, d: &Value) -> Result<(), Fault> {
        let hc = d
            .get("HostConfig")
            .and_then(Value::as_object)
            .ok_or(Fault)?;
        let config = d.get("Config").and_then(Value::as_object).ok_or(Fault)?;
        let expected_user = self.uid.to_string();
        if d.get("Image").and_then(Value::as_str) != Some(self.image.as_str())
            || config.get("User").and_then(Value::as_str) != Some(expected_user.as_str())
            || config.get("Entrypoint") != Some(&json!(["/opt/wolf/approved-entrypoint"]))
        {
            return Err(Fault);
        }
        for (key, wanted) in [
            ("Privileged", false),
            ("ReadonlyRootfs", true),
            ("PublishAllPorts", false),
        ] {
            if hc.get(key).and_then(Value::as_bool) != Some(wanted) {
                return Err(Fault);
            }
        }
        for (key, wanted) in [
            ("NetworkMode", "wolf-wow-egress"),
            ("IpcMode", "private"),
            ("PidMode", ""),
            ("UTSMode", ""),
            ("UsernsMode", ""),
            ("CgroupnsMode", "private"),
        ] {
            if hc.get(key).and_then(Value::as_str) != Some(wanted) {
                return Err(Fault);
            }
        }
        if caps(hc.get("CapDrop").ok_or(Fault)?)? != BTreeSet::from(["ALL".into()])
            || caps(hc.get("CapAdd").ok_or(Fault)?)?
                != BTreeSet::from(["MKNOD".into(), "NET_RAW".into(), "NET_ADMIN".into()])
            || strings(hc.get("SecurityOpt").ok_or(Fault)?)?
                != BTreeSet::from(["no-new-privileges".into()])
        {
            return Err(Fault);
        }
        if hc
            .get("PortBindings")
            .and_then(Value::as_object)
            .is_none_or(|p| !p.is_empty())
        {
            return Err(Fault);
        }
        match hc.get("DeviceRequests") {
            Some(Value::Null) => (),
            Some(Value::Array(v)) if v.is_empty() => (),
            _ => return Err(Fault),
        }
        match hc.get("DeviceCgroupRules") {
            Some(Value::Null) if self.rules.is_empty() => (),
            Some(v) if strings(v)? == self.rules.iter().cloned().collect() => (),
            _ => return Err(Fault),
        }
        let wanted_tmp = json!({"/tmp":"rw,nosuid,nodev,mode=1777",format!("/run/user/{}",self.uid):format!("rw,nosuid,nodev,mode=0700,uid={},gid={}",self.uid,self.uid)});
        if hc.get("Tmpfs") != Some(&wanted_tmp) {
            return Err(Fault);
        }
        let devices = hc.get("Devices").and_then(Value::as_array).ok_or(Fault)?;
        if devices.len() != self.gpu.len() {
            return Err(Fault);
        }
        let mut seen = BTreeSet::new();
        for device in devices {
            let host = device
                .get("PathOnHost")
                .and_then(Value::as_str)
                .ok_or(Fault)?;
            if device.get("PathInContainer").and_then(Value::as_str) != Some(host)
                || !self.gpu.iter().any(|p| p == host)
                || !seen.insert(host)
            {
                return Err(Fault);
            }
            permission(device.get("CgroupPermissions").ok_or(Fault)?)?;
        }
        let mut env = BTreeMap::new();
        for entry in config.get("Env").and_then(Value::as_array).ok_or(Fault)? {
            let (k, v) = entry
                .as_str()
                .and_then(|s| s.split_once('='))
                .ok_or(Fault)?;
            if env.insert(k, v).is_some() {
                return Err(Fault);
            }
        }
        let wayland = *env.get("WAYLAND_DISPLAY").ok_or(Fault)?;
        if wayland.len() > 96
            || !wayland.starts_with("wayland-")
            || !wayland
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"-_".contains(&b))
        {
            return Err(Fault);
        }
        let pulse = format!("{}/pulse-socket", self.runtime);
        let wayland = format!("{}/{wayland}", self.runtime);
        if env.get("XDG_RUNTIME_DIR").copied() != Some(self.runtime.as_str())
            || env.get("PULSE_SERVER").copied() != Some(pulse.as_str())
            || env.get("WINEPREFIX").copied() != Some("/home/retro/prefix")
            || env.get("HOME").copied() != Some("/home/retro")
            || env.get("DISPLAY").is_some_and(|s| !s.is_empty())
        {
            return Err(Fault);
        }
        let expected: BTreeMap<String, (String, bool)> = BTreeMap::from([
            ("/home/retro/prefix".into(), (self.prefix.clone(), true)),
            ("/games/wow".into(), (self.game.clone(), false)),
            (
                "/home/retro".into(),
                (format!("{}/client/WoW", self.state), true),
            ),
            (
                "/run/udev".into(),
                (format!("{}/client/WoW/udev", self.state), true),
            ),
            ("/usr/bin/fake-udev".into(), (self.helper.clone(), true)),
            (pulse.clone(), (pulse, true)),
            (wayland.clone(), (wayland, true)),
        ]);
        let mounts = d.get("Mounts").and_then(Value::as_array).ok_or(Fault)?;
        if mounts.len() != expected.len() {
            return Err(Fault);
        }
        let mut visited = BTreeSet::new();
        for m in mounts {
            let dest = m
                .get("Destination")
                .and_then(Value::as_str)
                .ok_or(Fault)?
                .trim_end_matches('/');
            let (source, rw) = expected.get(dest).ok_or(Fault)?;
            if m.get("Type").and_then(Value::as_str) != Some("bind")
                || m.get("Source").and_then(Value::as_str) != Some(source.as_str())
                || m.get("RW").and_then(Value::as_bool) != Some(*rw)
                || !visited.insert(dest)
            {
                return Err(Fault);
            }
        }
        let binds = hc.get("Binds").and_then(Value::as_array).ok_or(Fault)?;
        if binds.len() != expected.len() {
            return Err(Fault);
        }
        let mut seen = BTreeSet::new();
        for b in binds {
            let parts = b.as_str().ok_or(Fault)?.split(':').collect::<Vec<_>>();
            if parts.len() != 3 {
                return Err(Fault);
            }
            let dest = parts[1].trim_end_matches('/');
            let (source, rw) = expected.get(dest).ok_or(Fault)?;
            if parts[0] != source || parts[2] != if *rw { "rw" } else { "ro" } || !seen.insert(dest)
            {
                return Err(Fault);
            }
        }
        Ok(())
    }
}
