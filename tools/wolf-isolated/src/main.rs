use std::{env, path::Path, process::ExitCode};
fn main() -> ExitCode {
    let args: Vec<_> = env::args().collect();
    if args.len() == 4 && matches!(args[1].as_str(), "start" | "status" | "stop") {
        return match wolf_isolated::supervisor::command(&args[1], &args[2], &args[3]) {
            Ok(view) => {
                println!("{}", serde_json::to_string(&view).unwrap());
                if view.phase == wolf_isolated::runtime::Phase::Unknown {
                    ExitCode::from(4)
                } else if view.phase == wolf_isolated::runtime::Phase::Refused {
                    ExitCode::from(3)
                } else {
                    ExitCode::SUCCESS
                }
            }
            Err(_) => {
                eprintln!("runtime not provisioned, busy or ownership unknown; no new grant");
                ExitCode::from(3)
            }
        };
    }
    if args.len() == 2 && matches!(args[1].as_str(), "start" | "status" | "stop") {
        println!("{{\"runtime_supported\":false,\"error\":\"runtime_stage_not_implemented\"}}");
        return ExitCode::from(3);
    }
    if args.len() != 3 || !matches!(args[1].as_str(), "inspect" | "plan") {
        eprintln!("foundation only: wolf-isolated inspect|plan PROFILE.json; start/status/stop unsupported");
        return ExitCode::from(2);
    }
    match wolf_isolated::read_profile(Path::new(&args[2])) {
        Ok(p) => {
            let report = wolf_isolated::inspect(&p, args[1] == "plan");
            println!(
                "{}",
                serde_json::to_string_pretty(&report).expect("data-only report")
            );
            if report.file_observations_valid {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(2)
            }
        }
        Err(e) => {
            eprintln!("profile refused: {e}");
            ExitCode::from(2)
        }
    }
}
