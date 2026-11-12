//! Root-provisioned fixed contracts, not an inspect permission or a wire approval flag.
use crate::{bounded_path, hex, validate, Profile};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
pub const E1: &str = "e1fdfd2e12b79bfd30e6a776478ead29ce958e67";
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct FilePin {
    pub path: String,
    pub sha256: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Spec {
    pub schema: u32,
    pub profile_sha256: String,
    pub native_revision: String,
    pub config: FilePin,
    pub image_contract: FilePin,
    pub prefix_contract: FilePin,
    pub private_key: String,
    pub certificate: String,
    pub docker_socket: String,
    pub fake_udev: FilePin,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImageContract {
    pub image: String,
    pub image_id: String,
    pub native_revision: String,
    pub entrypoint_sha256: String,
    pub wine_uid: u32,
    pub wine_caps: Vec<String>,
    pub wine_no_new_privileges: bool,
    pub gstreamer_sha256: String,
    pub gpu_vendor: String,
    pub input_rule: FilePin,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PrefixContract {
    pub profile_sha256: String,
    pub source_dev: u64,
    pub source_ino: u64,
    pub working_dev: u64,
    pub working_ino: u64,
    pub clean_stop_receipt_sha256: String,
    pub migration_receipt_sha256: String,
    pub boot_id: String,
    pub original_processes: Vec<crate::runtime::ChildId>,
}
#[derive(Clone)]
pub struct Record {
    pub bytes: Vec<u8>,
    pub uid: u32,
    pub mode: u32,
}
pub trait Source {
    fn root_file(&mut self, path: &str, limit: u64) -> Result<Record, String>;
    fn private_dir(&mut self, path: &str, uid: u32) -> Result<(u64, u64), String>;
    fn socket(&mut self, path: &str, uid: Option<u32>) -> Result<(), String>;
    fn device(&mut self, path: &str, gid: u32) -> Result<(), String>;
    fn uid(&self) -> u32;
    fn directory_identity(&mut self, path: &str) -> Result<(u64, u64), String>;
    fn gpu_vendor(&mut self, node: &str) -> Result<String, String>;
    fn groups(&mut self) -> Result<Vec<u32>, String>;
    fn boot_id(&mut self) -> Result<String, String>;
    fn process_live(&mut self, id: &crate::runtime::ChildId) -> Result<bool, String>;
}
// Fields are crate-private: the executable can only construct this through prepare().
pub struct Prepared {
    pub(crate) spec: Spec,
    pub(crate) config: toml::Value,
    pub(crate) wolf: String,
    pub(crate) home: String,
    pub(crate) runtime: String,
    pub(crate) state: String,
    pub(crate) lease: String,
    pub(crate) prefix_identity: (u64, u64),
    pub(crate) profile_hash: String,
    pub(crate) spec_hash: String,
    pub(crate) plugins: String,
    pub(crate) uid: u32,
    pub(crate) image_id: String,
    pub(crate) image_ref: String,
}
fn fail() -> String {
    "not provisioned or incompatible fixed runtime contract".into()
}
pub fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn root<S: Source>(s: &mut S, path: &str, limit: u64) -> Result<Vec<u8>, String> {
    if !bounded_path(path) {
        return Err(fail());
    }
    let r = s.root_file(path, limit)?;
    if r.uid != 0 || r.mode & 0o022 != 0 || r.bytes.len() as u64 > limit {
        return Err(fail());
    }
    Ok(r.bytes)
}
fn pinned<S: Source>(s: &mut S, p: &FilePin, limit: u64) -> Result<Vec<u8>, String> {
    if !hex(&p.sha256) {
        return Err(fail());
    }
    let b = root(s, &p.path, limit)?;
    if digest(&b) != p.sha256 {
        return Err(fail());
    }
    Ok(b)
}
fn only(v: &toml::Value, keys: &[&str]) -> Result<(), String> {
    let t = v.as_table().ok_or_else(fail)?;
    if t.keys().any(|k| !keys.contains(&k.as_str())) {
        return Err(fail());
    }
    Ok(())
}
fn arr<'a>(v: &'a toml::Value, key: &str, n: usize) -> Result<&'a Vec<toml::Value>, String> {
    let a = v.get(key).and_then(|v| v.as_array()).ok_or_else(fail)?;
    if a.len() != n {
        return Err(fail());
    }
    Ok(a)
}
fn text<'a>(v: &'a toml::Value, key: &str) -> Result<&'a str, String> {
    v.get(key).and_then(|v| v.as_str()).ok_or_else(fail)
}
fn json_only(v: &Value, keys: &[&str]) -> Result<(), String> {
    if v.as_object()
        .ok_or_else(fail)?
        .keys()
        .any(|k| !keys.contains(&k.as_str()))
    {
        return Err(fail());
    }
    Ok(())
}
pub fn prepare<S: Source>(
    s: &mut S,
    profile_path: &str,
    spec_path: &str,
) -> Result<Prepared, String> {
    let pb = root(s, profile_path, crate::MAX_PROFILE)?;
    let p: Profile = crate::parse(&pb)?;
    if !validate(&p).is_empty() {
        return Err(fail());
    }
    let sb = root(s, spec_path, 32768)?;
    let spec: Spec = serde_json::from_slice(&sb).map_err(|_| fail())?;
    if spec.schema != 1
        || spec.native_revision != E1
        || spec.profile_sha256 != digest(&pb)
        || s.uid() != p.remote_uid
        || !["/run/docker.sock", "/var/run/docker.sock"].contains(&spec.docker_socket.as_str())
    {
        return Err(fail());
    }
    for a in &p.artifacts {
        pinned(
            s,
            &FilePin {
                path: a.path.clone(),
                sha256: a.sha256.clone(),
            },
            256 * 1024 * 1024,
        )?;
    }
    let image: ImageContract =
        serde_json::from_slice(&pinned(s, &spec.image_contract, 32768)?).map_err(|_| fail())?;
    let prefix: PrefixContract =
        serde_json::from_slice(&pinned(s, &spec.prefix_contract, 32768)?).map_err(|_| fail())?;
    if image.image != p.image
        || image.native_revision != E1
        || image
            .image_id
            .strip_prefix("sha256:")
            .is_none_or(|h| !hex(h))
        || image.wine_uid != p.remote_uid
        || !image.wine_caps.is_empty()
        || !image.wine_no_new_privileges
        || !["INTEL", "AMD"].contains(&image.gpu_vendor.as_str())
        || image.entrypoint_sha256
            != p.artifacts
                .iter()
                .find(|a| a.role == "entrypoint")
                .ok_or_else(fail)?
                .sha256
    {
        return Err(fail());
    }
    if image.input_rule.path != "/etc/udev/rules.d/72-wolf-wow.rules"
        || pinned(s, &image.input_rule, 16384)?
            != include_bytes!("../provisioning/72-wolf-wow.rules.in")
    {
        return Err(fail());
    }
    let users = root(s, "/etc/passwd", 256 * 1024)?;
    let users = std::str::from_utf8(&users).map_err(|_| fail())?;
    let remote = users
        .lines()
        .filter_map(|l| {
            let f = l.split(':').collect::<Vec<_>>();
            (f.len() >= 4 && f[2].parse::<u32>() == Ok(p.remote_uid)).then(|| f[0].to_owned())
        })
        .collect::<Vec<_>>();
    if remote.len() != 1 {
        return Err(fail());
    }
    let groups = root(s, "/etc/group", 256 * 1024)?;
    let groups = std::str::from_utf8(&groups).map_err(|_| fail())?;
    let mut required = vec![p.render_gid];
    for name in ["wolf-wow-input", "wolf-wow-uinput"] {
        let rows = groups
            .lines()
            .filter_map(|l| {
                let f = l.split(':').collect::<Vec<_>>();
                (f.len() == 4 && f[0] == name).then_some(f)
            })
            .collect::<Vec<_>>();
        if rows.len() != 1 || rows[0][3] != remote[0] {
            return Err(fail());
        }
        let gid = rows[0][2].parse::<u32>().map_err(|_| fail())?;
        if name == "wolf-wow-input" && gid != p.input_gid {
            return Err(fail());
        }
        required.push(gid);
        if name == "wolf-wow-uinput" {
            s.device("/dev/uinput", gid)?;
        }
    }
    let effective = s.groups()?;
    if effective.len() > 128 || effective.contains(&0) || effective.contains(&p.desktop_uid) {
        return Err(fail());
    }
    for line in groups.lines() {
        let fields = line.split(':').collect::<Vec<_>>();
        if fields.len() == 4
            && fields[0] == "input"
            && fields[2]
                .parse::<u32>()
                .is_ok_and(|g| effective.contains(&g))
        {
            return Err(fail());
        }
    }

    if required.iter().any(|g| !effective.contains(g)) {
        return Err(fail());
    }
    let state = format!("{}/state", p.remote_home);
    let lease = format!("{}/launcher", p.remote_home);
    for dir in [
        &p.remote_home,
        &p.runtime,
        &state,
        &lease,
        &format!("{state}/client"),
        &format!("{state}/client/WoW"),
    ] {
        s.private_dir(dir, p.remote_uid)?;
    }
    let working = s.private_dir(&p.prefix.working, p.remote_uid)?;
    if s.directory_identity(&p.prefix.source)? != (p.prefix.source_dev, p.prefix.source_ino)
        || s.gpu_vendor(&p.render_node)? != image.gpu_vendor
    {
        return Err(fail());
    }
    s.directory_identity(&p.game_data)?;
    if prefix.boot_id != s.boot_id()? || prefix.original_processes.len() > 4 {
        return Err(fail());
    }
    for process in &prefix.original_processes {
        if process.pid == 0 || process.start == 0 || s.process_live(process)? {
            return Err(fail());
        }
    }
    if prefix.profile_sha256 != digest(&pb)
        || prefix.source_dev != p.prefix.source_dev
        || prefix.source_ino != p.prefix.source_ino
        || working != (prefix.working_dev, prefix.working_ino)
        || !hex(&prefix.clean_stop_receipt_sha256)
        || !hex(&prefix.migration_receipt_sha256)
    {
        return Err(fail());
    }
    s.socket(&format!("{}/pulse-socket", p.runtime), Some(p.remote_uid))?;
    s.socket(&spec.docker_socket, None)?;
    s.device(&p.render_node, p.render_gid)?;
    if spec.fake_udev.path != format!("{state}/fake-udev") {
        return Err(fail());
    }
    pinned(s, &spec.fake_udev, 16 * 1024 * 1024)?;
    let key_record = s.root_file(&spec.private_key, 32768)?;
    if key_record.uid != 0
        || key_record.mode & 0o027 != 0
        || key_record.bytes.len() > 32768
        || !bounded_path(&spec.private_key)
    {
        return Err(fail());
    }
    root(s, &spec.certificate, 32768)?;
    let config_bytes = pinned(s, &spec.config, 256 * 1024)?;
    let config: toml::Value = std::str::from_utf8(&config_bytes)
        .map_err(|_| fail())?
        .parse()
        .map_err(|_| fail())?;
    validate_config(&p, &config, &image)?;
    // Host plugins are fixed independent root-provisioned files, not guest path assertions.
    let plugins = p
        .artifacts
        .iter()
        .filter(|a| a.role == "compositor" || a.role == "interpipe")
        .map(|a| {
            std::path::Path::new(&a.path)
                .parent()
                .unwrap()
                .to_string_lossy()
                .into_owned()
        })
        .collect::<Vec<_>>()
        .join(":");
    Ok(Prepared {
        wolf: p
            .artifacts
            .iter()
            .find(|a| a.role == "wolf")
            .unwrap()
            .path
            .clone(),
        home: p.remote_home,
        runtime: p.runtime,
        state,
        lease,
        prefix_identity: working,
        profile_hash: digest(&pb),
        spec_hash: digest(&sb),
        plugins,
        uid: p.remote_uid,
        image_id: image.image_id,
        image_ref: p.image,
        spec,
        config,
    })
}
pub fn validate_config(p: &Profile, c: &toml::Value, i: &ImageContract) -> Result<(), String> {
    only(
        c,
        &[
            "hostname",
            "uuid",
            "config_version",
            "support_hevc",
            "support_av1",
            "paired_clients",
            "profiles",
            "gstreamer",
        ],
    )?;
    if c.get("config_version").and_then(|v| v.as_integer()) != Some(7) {
        return Err(fail());
    }
    let client = &arr(c, "paired_clients", 1)?[0];
    only(client, &["client_cert", "app_state_folder", "settings"])?;
    if text(client, "app_state_folder")? != "client" || text(client, "client_cert")?.is_empty() {
        return Err(fail());
    }
    let settings = client.get("settings").ok_or_else(fail)?;
    only(
        settings,
        &[
            "run_uid",
            "run_gid",
            "controllers_override",
            "motion_controller_override",
            "mouse_acceleration",
            "v_scroll_acceleration",
            "h_scroll_acceleration",
        ],
    )?;
    if settings.get("run_uid").and_then(|v| v.as_integer()) != Some(p.remote_uid as i64)
        || settings.get("run_gid").and_then(|v| v.as_integer()) != Some(p.remote_uid as i64)
        || arr(settings, "controllers_override", 16)?
            .iter()
            .any(|v| v.as_str() != Some("XBOX"))
        || settings
            .get("motion_controller_override")
            .is_some_and(|v| v.as_str() != Some("AUTO"))
    {
        return Err(fail());
    }
    let profile = &arr(c, "profiles", 1)?[0];
    only(profile, &["id", "name", "apps"])?;
    if text(profile, "id")? != "moonlight-profile-id" {
        return Err(fail());
    }
    let app = &arr(profile, "apps", 1)?[0];
    only(
        app,
        &[
            "title",
            "start_virtual_compositor",
            "start_audio_server",
            "render_node",
            "runner",
            "device_free_diagnostic",
            "legacy_pad_only_trial",
            "legacy_direct_touch_trial",
            "legacy_keyboard_mouse_trial",
        ],
    )?;
    if text(app, "title")? != "WoW"
        || app
            .get("start_virtual_compositor")
            .and_then(|v| v.as_bool())
            != Some(true)
        || app.get("start_audio_server").and_then(|v| v.as_bool()) != Some(true)
        || text(app, "render_node")? != p.render_node
    {
        return Err(fail());
    }
    for flag in [
        "device_free_diagnostic",
        "legacy_pad_only_trial",
        "legacy_direct_touch_trial",
        "legacy_keyboard_mouse_trial",
    ] {
        if app.get(flag).is_some_and(|v| v.as_bool() != Some(false)) {
            return Err(fail());
        }
    }
    let runner = app.get("runner").ok_or_else(fail)?;
    only(
        runner,
        &[
            "type",
            "name",
            "image",
            "mounts",
            "env",
            "devices",
            "ports",
            "base_create_json",
        ],
    )?;
    if text(runner, "type")? != "docker"
        || text(runner, "name")? != "WolfWowIsolated"
        || text(runner, "image")? != p.image
    {
        return Err(fail());
    }
    arr(runner, "devices", 0)?;
    arr(runner, "ports", 0)?;
    let expected_mounts = [
        format!("{}:/home/retro/prefix:rw", p.prefix.working),
        format!("{}:/games/wow:ro", p.game_data),
    ];
    let mounts = arr(runner, "mounts", 2)?;
    if mounts
        .iter()
        .zip(&expected_mounts)
        .any(|(v, w)| v.as_str() != Some(w.as_str()))
    {
        return Err(fail());
    }
    let env = arr(runner, "env", 2)?;
    if env[0].as_str() != Some("WINEPREFIX=/home/retro/prefix")
        || env[1].as_str() != Some("HOME=/home/retro")
    {
        return Err(fail());
    }
    let j: Value = serde_json::from_str(text(runner, "base_create_json")?).map_err(|_| fail())?;
    json_only(&j, &["HostConfig", "User", "Entrypoint", "Labels"])?;
    let expected_user = p.remote_uid.to_string();
    if j["User"].as_str() != Some(expected_user.as_str())
        || j["Entrypoint"] != json!(["/opt/wolf/approved-entrypoint"])
        || j["Labels"] != json!({})
    {
        return Err(fail());
    }
    let h = &j["HostConfig"];
    json_only(
        h,
        &[
            "Privileged",
            "IpcMode",
            "NetworkMode",
            "PidMode",
            "CapDrop",
            "CapAdd",
            "SecurityOpt",
            "ReadonlyRootfs",
            "Tmpfs",
        ],
    )?;
    if h["Privileged"] != false
        || h["IpcMode"] != "private"
        || h["NetworkMode"] != "wolf-wow-egress"
        || h["PidMode"] != ""
        || h["CapDrop"] != json!(["ALL"])
        || h["CapAdd"] != json!(["MKNOD", "NET_RAW", "NET_ADMIN"])
        || h["SecurityOpt"] != json!(["no-new-privileges"])
        || h["ReadonlyRootfs"] != true
        || h["Tmpfs"]
            != json!({"/tmp":"rw,nosuid,nodev,mode=1777",format!("/run/user/{}",p.remote_uid):format!("rw,nosuid,nodev,mode=0700,uid={},gid={}",p.remote_uid,p.remote_uid)})
    {
        return Err(fail());
    }
    let gst = c.get("gstreamer").ok_or_else(fail)?;
    let value = serde_json::to_vec(gst).map_err(|_| fail())?;
    if digest(&value) != i.gstreamer_sha256 {
        return Err(fail());
    }
    // Native auto-mounts: state/client/WoW -> /home/retro, its udev subdir,
    // state/fake-udev executable and actual compositor/socket under private runtime.
    // No Nvidia driver-volume path: the provisioned fixed contract supports INTEL/AMD only.
    Ok(())
}
pub fn labeled_config(prepared: &Prepared, key: &crate::runtime::Key) -> Result<String, String> {
    let mut c = prepared.config.clone();
    let b = &mut c["profiles"][0]["apps"][0]["runner"]["base_create_json"];
    let mut j: Value = serde_json::from_str(b.as_str().ok_or_else(fail)?).map_err(|_| fail())?;
    j["Labels"] = json!({"mwow.nonce":key.nonce,"mwow.profile":key.profile});
    *b = toml::Value::String(serde_json::to_string(&j).map_err(|_| fail())?);
    toml::to_string(&c).map_err(|_| fail())
}
