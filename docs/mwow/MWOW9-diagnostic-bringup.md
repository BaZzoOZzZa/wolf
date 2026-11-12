# Diagnostic bring-up (not protected handoff)

Scope: operator-configured per-app device_free_diagnostic, default false. Internal App flag is skipped
in public serialization. Contradictory compositor/audio/non-process configuration fails before runner
construction. This controls Wolf-managed devices, not arbitrary operator commands or a host sandbox.

Production seam: sessions/diagnostic_startup.hpp owns the actual startup decision and future continuation
used by moonlight.cpp; injected operations stand for device factories/audio/producer/socket readiness,
not a copied lifecycle model. No locks are held across operations. Async continuation retains its
operations and captured session until completion. Compositor ready/error behavior stays separate from
no-compositor runner readiness. Existing runner and producer lifetimes are not redesigned.

Input policy is checked in the common bounded dispatcher, all direct side-effect helpers and lazy
factories. Exact-scope own Plug and lobby Join/Plug guards prevent diagnostic device attachment or alias.
Normal controller slots0..15, scope validation, MWOW6 decrypt/replay and F6/F7 guards are preserved.

Audio startup extraction keeps the existing actual Docker fallback behind a single explicit FALSE gate
before cleanup or Docker operations. Null/disconnected audio cannot dispatch router/handlers/ingress.
Default fallback remains enabled. WOLF_ENABLE_MDNS=FALSE skips the entire worker dispatch; default true
preserves behavior. Manager reported existing upstream mdns alignment UB: disabled discovery is a narrow
mitigation, NOT a fix/suppression of upstream UB. No dependency or sanitizer changes.

No deployment config/key/cert/UUID/state is changed here. Manager later reuses the SAME pairing identity,
adds only one fixed sleep600 ball/ticks app, and sets WOLF_ALLOW_PULSE_CONTAINER=FALSE,
WOLF_ENABLE_MDNS=FALSE with explicit existing Pulse. Live video, physical isolation/gamepad handoff and
WoW readiness are NOT proved by software tests. Independent review and Manager launch remain gates.
