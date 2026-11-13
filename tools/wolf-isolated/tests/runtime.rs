use serde_json::json;
use std::{
    collections::BTreeMap,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc,
    },
};
use wolf_isolated::{runtime::*, runtime_spec::*, Profile};
struct Files {
    files: BTreeMap<String, Record>,
    groups: Vec<u32>,
    uid: u32,
    known_live: bool,
}
impl Source for Files {
    fn root_file(&mut self, p: &str, n: u64) -> Result<Record, String> {
        let r = self.files.get(p).ok_or("missing")?.clone();
        if r.bytes.len() as u64 > n {
            return Err("bound".into());
        }
        Ok(r)
    }
    fn private_dir(&mut self, _: &str, u: u32) -> Result<(u64, u64), String> {
        if u != self.uid {
            return Err("uid".into());
        }
        Ok((1, 77))
    }
    fn directory_identity(&mut self, p: &str) -> Result<(u64, u64), String> {
        Ok(if p.contains("original-prefix") {
            (1, 1)
        } else {
            (1, 2)
        })
    }
    fn socket(&mut self, _: &str, _: Option<u32>) -> Result<(), String> {
        Ok(())
    }
    fn device(&mut self, _: &str, g: u32) -> Result<(), String> {
        if self.groups.contains(&g) {
            Ok(())
        } else {
            Err("missing group".into())
        }
    }
    fn gpu_nodes(&mut self, render: &str) -> Result<Vec<String>, String> {
        Ok(vec!["/dev/dri/card0".into(), render.into()])
    }
    fn gpu_vendor(&mut self, _: &str) -> Result<String, String> {
        Ok("INTEL".into())
    }
    fn boot_id(&mut self) -> Result<String, String> {
        Ok("12345678-1234-1234-1234-123456789012".into())
    }
    fn process_live(&mut self, _: &ChildId) -> Result<bool, String> {
        Ok(self.known_live)
    }
    fn uid(&self) -> u32 {
        self.uid
    }
    fn groups(&mut self) -> Result<Vec<u32>, String> {
        Ok(self.groups.clone())
    }
}
fn insert(files: &mut BTreeMap<String, Record>, p: &str, b: Vec<u8>) -> FilePin {
    let pin = FilePin {
        path: p.into(),
        sha256: digest(&b),
    };
    files.insert(
        p.into(),
        Record {
            bytes: b,
            uid: 0,
            mode: 0o100644,
        },
    );
    pin
}
fn config(p: &Profile) -> toml::Value {
    let h = json!({"Privileged":false,"IpcMode":"private","NetworkMode":"wolf-wow-egress","PidMode":"","CapDrop":["ALL"],"CapAdd":["MKNOD","NET_RAW","NET_ADMIN"],"SecurityOpt":["no-new-privileges"],"ReadonlyRootfs":true,"Tmpfs":{"/tmp":"rw,nosuid,nodev,mode=1777",format!("/run/user/{}",p.remote_uid):format!("rw,nosuid,nodev,mode=0700,uid={},gid={}",p.remote_uid,p.remote_uid)}});
    let v = json!({"config_version":7,"hostname":"fixture","uuid":"fixture-uuid","support_hevc":false,"support_av1":false,"paired_clients":[{"client_cert":"SOFTWARE FIXTURE NOT TLS PROOF","app_state_folder":"client","settings":{"run_uid":p.remote_uid,"run_gid":p.remote_uid,"controllers_override":vec!["XBOX";16]}}],"profiles":[{"id":"moonlight-profile-id","apps":[{"title":"WoW","start_virtual_compositor":true,"start_audio_server":true,"render_node":p.render_node,"runner":{"type":"docker","name":"WolfWowIsolated","image":p.image,"mounts":[format!("{}:/home/retro/prefix:rw",p.prefix.working),format!("{}:/games/wow:ro",p.game_data)],"devices":[],"ports":[],"env":["WINEPREFIX=/home/retro/prefix","HOME=/home/retro"],"base_create_json":json!({"HostConfig":h,"User":p.remote_uid.to_string(),"Entrypoint":["/opt/wolf/approved-entrypoint"],"Labels":{}}).to_string()}}]}],"gstreamer":{"video":{"default_source":"fixed fixture"}}});
    toml::Value::try_from(v).unwrap()
}
fn fixture() -> (Files, Profile, Spec, Key) {
    let mut p: Profile =
        wolf_isolated::parse(include_bytes!("../profiles/wow.example.json")).unwrap();
    let mut files = BTreeMap::new();
    for a in &mut p.artifacts {
        let pin = insert(&mut files, &a.path, a.role.as_bytes().to_vec());
        a.sha256 = pin.sha256;
    }
    let pb = serde_json::to_vec(&p).unwrap();
    insert(&mut files, "/provision/profile.json", pb.clone());
    let rule = insert(
        &mut files,
        "/etc/udev/rules.d/72-wolf-wow.rules",
        include_bytes!("../provisioning/72-wolf-wow.rules.in").to_vec(),
    );
    insert(&mut files,"/etc/passwd",b"remote:x:2001:2001::/srv/wolf-wow:/bin/false\ndesktop:x:1000:1000::/home/desktop:/bin/sh\n".to_vec());
    insert(
        &mut files,
        "/etc/group",
        b"wolf-wow-input:x:2002:remote\nwolf-wow-uinput:x:2004:remote\n".to_vec(),
    );
    insert(
        &mut files,
        "/proc/devices",
        b"Character devices:\n13 input\n240 hidraw\nBlock devices:\n".to_vec(),
    );
    let c = config(&p);
    let image = ImageContract {
        image: p.image.clone(),
        image_id: format!("sha256:{}", "a".repeat(64)),
        native_revision: E1.into(),
        entrypoint_sha256: p
            .artifacts
            .iter()
            .find(|a| a.role == "entrypoint")
            .unwrap()
            .sha256
            .clone(),
        wine_uid: p.remote_uid,
        wine_caps: vec![],
        wine_no_new_privileges: true,
        gstreamer_sha256: digest(&serde_json::to_vec(&c["gstreamer"]).unwrap()),
        gpu_vendor: "INTEL".into(),
        input_rule: rule,
    };
    let prefix = PrefixContract {
        profile_sha256: digest(&pb),
        source_dev: 1,
        source_ino: 1,
        working_dev: 1,
        working_ino: 77,
        clean_stop_receipt_sha256: "b".repeat(64),
        migration_receipt_sha256: "c".repeat(64),
        boot_id: "12345678-1234-1234-1234-123456789012".into(),
        original_processes: vec![ChildId {
            pid: 500,
            start: 100,
        }],
    };
    let s = Spec {
        schema: 1,
        profile_sha256: digest(&pb),
        native_revision: E1.into(),
        config: insert(
            &mut files,
            "/provision/wolf.toml",
            toml::to_string(&c).unwrap().into_bytes(),
        ),
        image_contract: insert(
            &mut files,
            "/provision/image.json",
            serde_json::to_vec(&image).unwrap(),
        ),
        prefix_contract: insert(
            &mut files,
            "/provision/prefix.json",
            serde_json::to_vec(&prefix).unwrap(),
        ),
        private_key: "/provision/key".into(),
        certificate: "/provision/cert".into(),
        docker_socket: "/run/docker.sock".into(),
        fake_udev: insert(
            &mut files,
            "/srv/wolf-wow/state/fake-udev",
            b"helper fixture".to_vec(),
        ),
    };
    insert(&mut files, "/provision/key", b"not a real key".to_vec());
    files.get_mut("/provision/key").unwrap().mode = 0o100640;
    insert(&mut files, "/provision/cert", b"not a real cert".to_vec());
    files.get_mut("/provision/key").unwrap().mode = 0o100640;
    let sb = serde_json::to_vec(&s).unwrap();
    insert(&mut files, "/provision/spec.json", sb.clone());
    let key = Key {
        boot: "12345678-1234-1234-1234-123456789012".into(),
        pid: 123,
        start: 321,
        nonce: "d".repeat(64),
        profile: digest(&pb),
        spec: digest(&sb),
    };
    (
        Files {
            files,
            groups: vec![2002, 2003, 2004],
            uid: 2001,
            known_live: false,
        },
        p,
        s,
        key,
    )
}
#[derive(Clone, Copy)]
enum Spawn {
    Healthy,
    Fail,
    Unknown,
}
struct Mock {
    busy: Arc<AtomicBool>,
    spawns: Arc<AtomicUsize>,
    stops: Vec<ChildId>,
    guests: Vec<Guest>,
    child_exit: bool,
    engine_error: bool,
    spawn: Spawn,
    releases: usize,
    saves: usize,
    save_error: bool,
    now_ms: u64,
}
impl Mock {
    fn new() -> Self {
        Self {
            busy: Arc::new(AtomicBool::new(false)),
            spawns: Arc::new(AtomicUsize::new(0)),
            stops: vec![],
            guests: vec![],
            child_exit: false,
            engine_error: false,
            spawn: Spawn::Healthy,
            releases: 0,
            saves: 0,
            save_error: false,
            now_ms: 0,
        }
    }
    fn guest(k: &Key, state: GuestState) -> Guest {
        Guest {
            id: "e".repeat(64),
            image: format!("sha256:{}", "a".repeat(64)),
            nonce: k.nonce.clone(),
            profile: k.profile.clone(),
            state,
        }
    }
}
impl Io for Mock {
    fn monotonic(&mut self) -> Result<std::time::Duration, Fault> {
        Ok(std::time::Duration::from_millis(self.now_ms))
    }
    fn reserve(&mut self, _: &Key) -> Result<(), Fault> {
        self.busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .map(|_| ())
            .map_err(|_| Fault)
    }
    fn save(&mut self, _: &Journal) -> Result<(), Fault> {
        self.saves += 1;
        if self.save_error {
            Err(Fault)
        } else {
            Ok(())
        }
    }
    fn spawn(&mut self, _: &Prepared) -> Result<ChildId, SpawnError> {
        self.spawns.fetch_add(1, Ordering::SeqCst);
        match self.spawn {
            Spawn::Healthy => Ok(ChildId { pid: 42, start: 77 }),
            Spawn::Fail => Err(SpawnError::NoChild),
            Spawn::Unknown => Err(SpawnError::Uncertain(Some(ChildId { pid: 42, start: 77 }))),
        }
    }
    fn guests(&mut self, _: &Key) -> Result<Vec<Guest>, Fault> {
        if self.engine_error {
            Err(Fault)
        } else {
            Ok(self.guests.clone())
        }
    }
    fn child_wait(&mut self, c: &ChildId) -> Result<Option<i32>, Fault> {
        assert_eq!(c, &ChildId { pid: 42, start: 77 });
        Ok(self.child_exit.then_some(0))
    }
    fn graceful_stop(&mut self, c: &ChildId) -> Result<(), Fault> {
        self.stops.push(c.clone());
        Ok(())
    }
    fn release(&mut self, _: &Key) -> Result<(), Fault> {
        self.releases += 1;
        self.busy.store(false, Ordering::SeqCst);
        Ok(())
    }
}
fn ready() -> (Prepared, Key) {
    let (mut f, _, _, k) = fixture();
    (
        prepare(&mut f, "/provision/profile.json", "/provision/spec.json").unwrap(),
        k,
    )
}
#[test]
fn actual_parsers_and_missing_prerequisites_precede_effects() {
    for missing in [
        "/provision/image.json",
        "/provision/prefix.json",
        "/provision/wolf.toml",
        "/etc/udev/rules.d/72-wolf-wow.rules",
        "/srv/wolf-wow/state/fake-udev",
    ] {
        let (mut f, _, _, k) = fixture();
        f.files.remove(missing);
        let io = Mock::new();
        let effects = io.spawns.clone();
        if let Ok(p) = prepare(&mut f, "/provision/profile.json", "/provision/spec.json") {
            let _ = Controller::start(io, &p, k);
        }
        assert_eq!(effects.load(Ordering::SeqCst), 0);
    }
    let (mut f, _, _, _) = fixture();
    f.files.get_mut("/provision/profile.json").unwrap().uid = 1000;
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
    let (mut f, _, _, _) = fixture();
    f.files
        .get_mut("/etc/group")
        .unwrap()
        .bytes
        .extend_from_slice(b"wolf-wow-input:x:2002:desktop\n");
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
    let (mut f, _, _, _) = fixture();
    f.groups.retain(|g| *g != 2004);
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
}
#[test]
fn config_rejects_host_overrides_and_separates_helper_from_wine_caps() {
    let (_, p, _, _) = fixture();
    let base = config(&p);
    let mut image: ImageContract =
        serde_json::from_slice(&fixture().0.files["/provision/image.json"].bytes).unwrap();
    assert!(validate_config(&p, &base, &image).is_ok());
    for mutation in 0..9 {
        let mut c = base.clone();
        match mutation {
            0 => c["profiles"][0]["apps"][0]["start_virtual_compositor"] = false.into(),
            1 => {
                c["profiles"][0]["apps"][0]
                    .as_table_mut()
                    .unwrap()
                    .insert("legacy_direct_touch_trial".into(), true.into());
            }
            2 => {
                c["profiles"][0]["apps"][0]["runner"]["mounts"] =
                    toml::Value::Array(vec!["/home:/games/wow:ro".into()])
            }
            3 => {
                c["profiles"][0]["apps"][0]["runner"]["devices"] =
                    toml::Value::Array(vec!["/dev/input".into()])
            }
            4 => c["profiles"][0]["apps"][0]["runner"]["type"] = "process".into(),
            5 => c["paired_clients"][0]["settings"]["run_uid"] = 0.into(),
            6 => {
                c["paired_clients"][0]["settings"]["controllers_override"] =
                    toml::Value::Array(vec!["PS".into()])
            }
            7 => {
                let a = c["profiles"][0]["apps"][0].clone();
                c["profiles"][0]["apps"].as_array_mut().unwrap().push(a)
            }
            _ => c["profiles"][0]["apps"][0]["runner"]["base_create_json"] = "{}".into(),
        };
        assert!(validate_config(&p, &c, &image).is_err());
    }
    image.wine_caps.push("MKNOD".into());
    let (mut f, _, mut s, _) = fixture();
    s.image_contract = insert(
        &mut f.files,
        "/provision/image.json",
        serde_json::to_vec(&image).unwrap(),
    );
    insert(
        &mut f.files,
        "/provision/spec.json",
        serde_json::to_vec(&s).unwrap(),
    );
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
}
#[test]
fn generated_native_config_labels_are_real_json_not_substrings() {
    let (p, k) = ready();
    let c: toml::Value = labeled_config(&p, &k).unwrap().parse().unwrap();
    let j: serde_json::Value = serde_json::from_str(
        c["profiles"][0]["apps"][0]["runner"]["base_create_json"]
            .as_str()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(j["Labels"]["mwow.nonce"], k.nonce);
    assert_eq!(j["HostConfig"]["CapDrop"], json!(["ALL"]));
    assert_eq!(j["User"], "2001");
}
#[test]
fn busy_and_foreign_key_do_not_spawn() {
    let (p, k) = ready();
    let io = Mock::new();
    io.busy.store(true, Ordering::SeqCst);
    let count = io.spawns.clone();
    assert!(Controller::start(io, &p, k.clone()).is_err());
    assert_eq!(count.load(Ordering::SeqCst), 0);
    let io = Mock::new();
    let count = io.spawns.clone();
    let mut bad = k;
    bad.profile = "f".repeat(64);
    assert!(Controller::start(io, &p, bad).is_err());
    assert_eq!(count.load(Ordering::SeqCst), 0);
}
#[test]
fn no_child_spawn_rollback_and_partial_spawn_unknown_are_distinct() {
    let (p, k) = ready();
    let mut io = Mock::new();
    io.spawn = Spawn::Fail;
    let c = Controller::start(io, &p, k.clone()).ok().unwrap();
    assert_eq!(c.journal.phase, Phase::Refused);
    assert_eq!(c.io.releases, 1);
    let mut io = Mock::new();
    io.spawn = Spawn::Unknown;
    let mut c = Controller::start(io, &p, k).ok().unwrap();
    assert_eq!(c.journal.phase, Phase::Unknown);
    assert_eq!(c.io.releases, 0);
    let key = c.journal.key.clone();
    assert!(c.stop(&key));
    assert_eq!(c.io.stops, vec![ChildId { pid: 42, start: 77 }]);
}
#[test]
fn positive_known_exact_exit_requires_running_history_and_actual_wait() {
    let (p, k) = ready();
    let mut c = Controller::start(Mock::new(), &p, k.clone()).ok().unwrap();
    c.io.guests = vec![Mock::guest(&k, GuestState::Running)];
    assert_eq!(c.tick(), Phase::Running);
    c.io.guests[0].state = GuestState::Exited;
    assert_ne!(c.tick(), Phase::Stopped);
    assert_eq!(c.io.releases, 0);
    c.io.child_exit = true;
    assert_eq!(c.tick(), Phase::Stopped);
    assert_eq!(c.io.releases, 1);
}
#[test]
fn no_cid_engine_loss_multiple_and_changed_identity_retain_unknown() {
    for failure in 0..6 {
        let (p, k) = ready();
        let mut c = Controller::start(Mock::new(), &p, k.clone()).ok().unwrap();
        match failure {
            0 => c.io.child_exit = true,
            1 => c.io.engine_error = true,
            2 => c.io.guests = vec![Mock::guest(&k, GuestState::Running); 2],
            3 => {
                let mut g = Mock::guest(&k, GuestState::Running);
                g.nonce = "foreign".into();
                c.io.guests = vec![g]
            }
            4 => {
                c.io.guests = vec![Mock::guest(&k, GuestState::Exited)];
                c.io.child_exit = true
            }
            _ => c.io.save_error = true,
        };
        assert_eq!(c.tick(), Phase::Unknown);
        c.io.engine_error = false;
        c.io.save_error = false;
        c.io.guests = vec![Mock::guest(&k, GuestState::Running)];
        c.tick();
        c.io.guests[0].state = GuestState::Exited;
        c.io.child_exit = true;
        assert_eq!(c.tick(), Phase::Unknown);
        assert_eq!(c.io.releases, 0);
    }
}
#[test]
fn foreign_boot_pid_and_nonce_stop_never_target_minecraft() {
    let (p, k) = ready();
    let mut c = Controller::start(Mock::new(), &p, k.clone()).ok().unwrap();
    for field in 0..4 {
        let mut bad = k.clone();
        match field {
            0 => bad.boot = "other-boot".into(),
            1 => bad.pid = 397943,
            2 => bad.start += 1,
            _ => bad.nonce = "f".repeat(64),
        };
        assert!(!c.stop(&bad));
    }
    assert!(c.io.stops.is_empty());
    assert!(c.stop(&k));
    assert_eq!(c.io.stops, vec![ChildId { pid: 42, start: 77 }]);
    c.deadline();
    assert_eq!(c.journal.phase, Phase::Unknown);
}
#[test]
fn concurrent_lease_reservation_only_one_actual_spawn() {
    let (p, k) = ready();
    let p = Arc::new(p);
    let barrier = Arc::new(std::sync::Barrier::new(2));
    let busy = Arc::new(AtomicBool::new(false));
    let count = Arc::new(AtomicUsize::new(0));
    let mut threads = vec![];
    for _ in 0..2 {
        let (p, k, b, busy, count) = (
            p.clone(),
            k.clone(),
            barrier.clone(),
            busy.clone(),
            count.clone(),
        );
        threads.push(std::thread::spawn(move || {
            let mut io = Mock::new();
            io.busy = busy;
            io.spawns = count;
            let result = Controller::start(io, &p, k);
            b.wait();
            result.is_ok()
        }));
    }
    let results = threads
        .into_iter()
        .map(|t| t.join().unwrap())
        .collect::<Vec<_>>();
    assert_eq!(results.iter().filter(|v| **v).count(), 1);
    assert_eq!(count.load(Ordering::SeqCst), 1);
}
#[test]
fn pure_cli_unprovisioned_refusal_has_no_daemon_or_device_effects() {
    let output = std::process::Command::new(env!("CARGO_BIN_EXE_wolf-isolated"))
        .args([
            "start",
            "/MWOW9-NOT-PROVISIONED/profile.json",
            "/MWOW9-NOT-PROVISIONED/spec.json",
        ])
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(3));
    assert!(output.stdout.is_empty());
    assert!(String::from_utf8(output.stderr)
        .unwrap()
        .contains("not provisioned"));
}

#[test]
fn known_live_original_process_and_stale_boot_refuse_before_spawn() {
    let (mut f, _, _, _) = fixture();
    f.known_live = true;
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
    let (mut f, _, mut s, _) = fixture();
    let mut c: PrefixContract =
        serde_json::from_slice(&f.files["/provision/prefix.json"].bytes).unwrap();
    c.boot_id = "old-boot".into();
    s.prefix_contract = insert(
        &mut f.files,
        "/provision/prefix.json",
        serde_json::to_vec(&c).unwrap(),
    );
    insert(
        &mut f.files,
        "/provision/spec.json",
        serde_json::to_vec(&s).unwrap(),
    );
    assert!(prepare(&mut f, "/provision/profile.json", "/provision/spec.json").is_err());
}

#[test]
fn actual_single_deadline_does_not_reset_and_backward_clock_fails_closed() {
    let (p, k) = ready();
    let mut c = Controller::start(Mock::new(), &p, k.clone()).ok().unwrap();
    c.io.guests = vec![Mock::guest(&k, GuestState::Running)];
    c.tick();
    assert!(c.stop(&k));
    c.io.now_ms = 4999;
    assert_ne!(c.tick(), Phase::Unknown);
    c.stop(&k);
    c.io.now_ms = 5000;
    assert_eq!(c.tick(), Phase::Unknown);
    c.io.child_exit = true;
    c.io.guests[0].state = GuestState::Exited;
    assert_eq!(c.tick(), Phase::Unknown);
    assert_eq!(c.io.releases, 0);
    let mut io = Mock::new();
    io.now_ms = 1000;
    let mut c = Controller::start(io, &p, k.clone()).ok().unwrap();
    c.stop(&k);
    c.io.now_ms = 999;
    assert_eq!(c.tick(), Phase::Unknown);
}

#[test]
fn actual_inspect_decoder_requires_exact_terminal_task_state() {
    let id = "a".repeat(64);
    let (prepared, _) = ready();
    let mut d = valid_inspection(&id, "running");
    assert_eq!(
        wolf_isolated::supervisor::decode_inspection(&d, &id, prepared.inspection_policy())
            .unwrap()
            .state,
        GuestState::Running
    );
    d["State"]["Status"] = "exited".into();
    assert_eq!(
        wolf_isolated::supervisor::decode_inspection(&d, &id, prepared.inspection_policy())
            .unwrap()
            .state,
        GuestState::Other
    );
    d["State"]["Running"] = false.into();
    d["State"]["Pid"] = 0.into();
    assert_eq!(
        wolf_isolated::supervisor::decode_inspection(&d, &id, prepared.inspection_policy())
            .unwrap()
            .state,
        GuestState::Exited
    );
    d["State"]["Dead"] = true.into();
    assert_eq!(
        wolf_isolated::supervisor::decode_inspection(&d, &id, prepared.inspection_policy())
            .unwrap()
            .state,
        GuestState::Other
    );
    assert!(wolf_isolated::supervisor::decode_inspection(
        &d,
        &"c".repeat(64),
        prepared.inspection_policy()
    )
    .is_err());
}

#[test]
fn inspect_decoder_rejects_host_escape_hatches() {
    let id = "a".repeat(64);
    let (prepared, _) = ready();
    let base = valid_inspection(&id, "exited");
    assert_eq!(
        wolf_isolated::supervisor::decode_inspection(&base, &id, prepared.inspection_policy())
            .unwrap()
            .state,
        GuestState::Exited
    );
    for (ptr, v) in [
        ("/HostConfig/Privileged", json!(true)),
        ("/HostConfig/NetworkMode", json!("host")),
        ("/HostConfig/PidMode", json!("host")),
        ("/Mounts/0/Source", json!("/var/run/docker.sock")),
        ("/Mounts/0/Source", json!("/tmp/.X11-unix")),
        ("/Mounts/0/Source", json!("/dev/input/event3")),
    ] {
        let mut d = base.clone();
        if ptr.ends_with("PidMode") {
            d["HostConfig"]["PidMode"] = v;
        } else {
            *d.pointer_mut(ptr).unwrap() = v;
        }
        assert!(
            wolf_isolated::supervisor::decode_inspection(&d, &id, prepared.inspection_policy())
                .is_err(),
            "{ptr}"
        );
    }
}

fn valid_inspection(id: &str, state: &str) -> serde_json::Value {
    let (_, p, _, _) = fixture();
    let c = config(&p);
    let mut j: serde_json::Value = serde_json::from_str(
        c["profiles"][0]["apps"][0]["runner"]["base_create_json"]
            .as_str()
            .unwrap(),
    )
    .unwrap();
    for name in ["UTSMode", "UsernsMode"] {
        j["HostConfig"][name] = json!("");
    }
    j["HostConfig"]["DeviceCgroupRules"] = json!(["c 240:* rwm", "c 13:* rwm"]);
    j["HostConfig"]["Devices"] = json!([{"PathOnHost":"/dev/dri/card0","PathInContainer":"/dev/dri/card0","CgroupPermissions":"mrw"},{"PathOnHost":"/dev/dri/renderD128","PathInContainer":"/dev/dri/renderD128","CgroupPermissions":"mrw"}]);
    j["HostConfig"]["DeviceRequests"] = json!(null);
    j["HostConfig"]["PortBindings"] = json!({});
    let mounts = json!([
 {"Type":"bind","Source":"/srv/wolf-wow/prefix","Destination":"/home/retro/prefix","RW":true},
 {"Type":"bind","Source":"/srv/wow-game-data","Destination":"/games/wow","RW":false},
 {"Type":"bind","Source":"/srv/wolf-wow/state/client/WoW","Destination":"/home/retro","RW":true},
 {"Type":"bind","Source":"/srv/wolf-wow/state/client/WoW/udev","Destination":"/run/udev","RW":true},
 {"Type":"bind","Source":"/srv/wolf-wow/state/fake-udev","Destination":"/usr/bin/fake-udev","RW":true},
 {"Type":"bind","Source":"/run/user/2001/pulse-socket","Destination":"/run/user/2001/pulse-socket","RW":true},
 {"Type":"bind","Source":"/run/user/2001/wayland-7","Destination":"/run/user/2001/wayland-7","RW":true}]);
    json!({"Id":id,"Image":format!("sha256:{}","a".repeat(64)),"Config":{"Labels":{"mwow.nonce":"n","mwow.profile":"p"},"User":"2001","Entrypoint":["/opt/wolf/approved-entrypoint"],"Env":["WAYLAND_DISPLAY=wayland-7","PULSE_SERVER=/run/user/2001/pulse-socket","XDG_RUNTIME_DIR=/run/user/2001"]},"State":{"Status":state,"Running":state=="running","Pid":if state=="running"{100}else{0},"Paused":false,"Restarting":false,"Dead":false},"HostConfig":j["HostConfig"],"Mounts":mounts})
}
#[test]
fn r39_inspection_requires_mandatory_expected_envelope() {
    let (p, _) = ready();
    let id = "a".repeat(64);
    let base = valid_inspection(&id, "exited");
    let mut accepted = Vec::new();
    for (name, ptr, value) in [
        ("missing HC", "/HostConfig", json!(null)),
        ("wrong HC", "/HostConfig", json!([])),
        ("missing mounts", "/Mounts", json!(null)),
        ("wrong mounts", "/Mounts", json!({})),
        (
            "foreign net",
            "/HostConfig/NetworkMode",
            json!("container:foreign"),
        ),
        (
            "foreign ipc",
            "/HostConfig/IpcMode",
            json!("container:foreign"),
        ),
        (
            "foreign pid",
            "/HostConfig/PidMode",
            json!("container:foreign"),
        ),
        ("extra cap", "/HostConfig/CapAdd", json!(["SYS_ADMIN"])),
        ("wrong user", "/Config/User", json!("0")),
        (
            "extra mount",
            "/Mounts/0/Source",
            json!("/srv/wolf-wow/other"),
        ),
        ("missing devices", "/HostConfig/Devices", json!(null)),
    ] {
        let mut d = base.clone();
        *d.pointer_mut(ptr).unwrap() = value;
        if wolf_isolated::supervisor::decode_inspection(&d, &id, p.inspection_policy()).is_ok() {
            accepted.push(name);
        }
    }
    assert!(
        accepted.is_empty(),
        "incomplete/foreign envelopes accepted: {accepted:?}"
    );
}
