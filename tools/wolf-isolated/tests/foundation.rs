use wolf_isolated::{inspect, parse, validate, Profile, MAX_PROFILE};
fn profile() -> Profile {
    parse(include_bytes!("../profiles/wow.example.json")).unwrap()
}
#[test]
fn fixed_example_is_proposal_not_ready() {
    let p = profile();
    assert!(validate(&p).is_empty());
    let r = inspect(&p, true);
    assert!(!r.runtime_supported);
    assert!(!r.file_observations_valid);
    assert!(r.errors.iter().any(|e| e.contains("artifact")));
    let a = r.proposal.unwrap().docker_create_argv;
    for arg in [
        "--group-add=2002",
        "--group-add=2003",
        "--cap-drop=ALL",
        "--security-opt=no-new-privileges",
        "--network=wolf-wow-egress",
        "--ipc=private",
        "--read-only",
    ] {
        assert!(a.iter().any(|a| a == arg));
    }
    assert!(!a
        .iter()
        .any(|a| a.contains("/tmp/.X11-unix") || a.contains("docker.sock")));
    assert!(a.iter().any(|a| a.contains("dst=/home/wolf/prefix")));
}
#[test]
fn bounded_parse_unknown_fields_and_duplicate_fields() {
    assert!(parse(&vec![b' '; MAX_PROFILE as usize + 1]).is_err());
    assert!(parse(br#"{"schema":1,"schema":1}"#).is_err());
    let mut j = serde_json::to_value(profile()).unwrap();
    j["execute"] = "sh".into();
    assert!(parse(&serde_json::to_vec(&j).unwrap()).is_err());
}
#[test]
fn wrong_uid_caps_and_mknod_refused() {
    for change in 0..7 {
        let mut p = profile();
        match change {
            0 => p.remote_uid = 0,
            1 => p.remote_uid = p.desktop_uid,
            2 => p.capabilities.push("ALL".into()),
            3 => p.allow_mknod = true,
            4 => p.private_dev = false,
            5 => p.host_network = true,
            _ => p.host_ipc = true,
        };
        assert!(!validate(&p).is_empty());
        assert!(inspect(&p, true).proposal.is_none());
    }
}
#[test]
fn host_x11_runtime_input_and_docker_mounts_refused() {
    for source in [
        "/tmp/.X11-unix",
        "/run/user/1000",
        "/dev/input",
        "/run/docker.sock",
        "/var/run/docker.sock",
        "/home/desktop",
        "/",
        "/srv/games/../etc",
        "/srv/games\n",
    ] {
        let mut p = profile();
        p.game_data = source.into();
        assert!(!validate(&p).is_empty(), "{source}");
    }
    let mut p = profile();
    p.runtime = "/run/user/1000".into();
    assert!(!validate(&p).is_empty());
}
#[test]
fn exact_nodes_only_no_broad_or_uhid_devices() {
    for node in [
        "/dev/input",
        "/dev/uinput",
        "/dev/uhid",
        "/dev/input/event1:rw",
        "/dev/input/event-1",
    ] {
        let mut p = profile();
        p.input_node = node.into();
        assert!(!validate(&p).is_empty());
    }
}
#[test]
fn prefix_identity_and_alias_refusal() {
    let mut bad_group = profile();
    bad_group.input_gid = 0;
    assert!(!validate(&bad_group).is_empty());
    let mut p = profile();
    p.prefix.source_ino = 0;
    assert!(!validate(&p).is_empty());
    let mut p = profile();
    p.prefix.working = p.prefix.source.clone();
    assert!(!validate(&p).is_empty());
    let mut p = profile();
    p.prefix.wine_arch = "win32".into();
    assert!(!validate(&p).is_empty());
}
#[test]
fn missing_unpinned_duplicate_artifacts_and_image_refused() {
    let mut p = profile();
    p.artifacts.pop();
    assert!(!validate(&p).is_empty());
    let mut p = profile();
    p.artifacts[1].role = "wolf".into();
    assert!(!validate(&p).is_empty());
    let mut p = profile();
    p.artifacts[0].sha256 = "latest".into();
    assert!(!validate(&p).is_empty());
    let mut p = profile();
    p.image = "wow:latest".into();
    assert!(!validate(&p).is_empty());
}
#[test]
fn runtime_commands_explicitly_unsupported_without_effects() {
    for action in ["start", "status", "stop"] {
        let out = std::process::Command::new(env!("CARGO_BIN_EXE_wolf-isolated"))
            .arg(action)
            .output()
            .unwrap();
        assert_eq!(out.status.code(), Some(3));
        let v: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
        assert_eq!(v["runtime_supported"], false);
        assert_eq!(v["error"], "runtime_stage_not_implemented");
    }
}

#[test]
fn game_tree_overlap_both_directions_and_sibling() {
    let mut accepted = Vec::new();
    for source in [
        "/home",
        "/home/desktop",
        "/home/desktop/games",
        "/srv",
        "/srv/wolf-wow",
        "/srv/wolf-wow/games",
        "/APPROVAL_REQUIRED",
        "/APPROVAL_REQUIRED/original-prefix",
        "/APPROVAL_REQUIRED/original-prefix/games",
    ] {
        let mut p = profile();
        p.game_data = source.into();
        if validate(&p).is_empty() || inspect(&p, true).proposal.is_some() {
            accepted.push(source);
        }
    }
    assert!(
        accepted.is_empty(),
        "overlapping game trees accepted: {accepted:?}"
    );
    for source in [
        "/home/desktop-games",
        "/srv/wolf-wow-games",
        "/APPROVAL_REQUIRED/original-prefix-games",
    ] {
        let mut p = profile();
        p.game_data = source.into();
        assert!(
            validate(&p).is_empty(),
            "distinct sibling refused: {source}"
        );
        assert!(inspect(&p, true).proposal.is_some());
    }
}

#[test]
fn cli_plan_refuses_ancestor_mounts_but_keeps_distinct_sibling_proposal() {
    let root = std::path::PathBuf::from(std::env::var_os("CARGO_TARGET_DIR").unwrap());
    let file = root.join(format!("r37-profile-{}.json", std::process::id()));
    assert!(!file.exists());
    struct Cleanup(std::path::PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            std::fs::remove_file(&self.0).unwrap();
        }
    }
    std::fs::write(&file, b"").unwrap();
    let _cleanup = Cleanup(file.clone());
    let mut accepted = Vec::new();
    for source in ["/home", "/srv", "/APPROVAL_REQUIRED", "/home/desktop-games"] {
        let mut p = profile();
        p.game_data = source.into();
        std::fs::write(&file, serde_json::to_vec(&p).unwrap()).unwrap();
        let output = std::process::Command::new(env!("CARGO_BIN_EXE_wolf-isolated"))
            .arg("plan")
            .arg(&file)
            .output()
            .unwrap();
        assert_eq!(output.status.code(), Some(2)); // placeholders remain unprovisioned
        let report: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
        assert_eq!(report["runtime_supported"], false);
        if source == "/home/desktop-games" {
            assert_eq!(report["manifest_valid"], true);
            assert!(report["proposal"].is_object());
        } else if report["manifest_valid"] != false || !report["proposal"].is_null() {
            accepted.push(source);
        }
    }
    assert!(
        accepted.is_empty(),
        "CLI proposed ancestor mounts: {accepted:?}"
    );
}
