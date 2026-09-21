# macOS streaming lock-state decision

- Required behavior: lock/suspend/session loss ends capture, file playback, audio and control; returning requires a fresh visible offer/consent.
- Current source: `MacSessionEnvironment.mm` watches workspace sleep/deactivation and checks the current session dictionary. `CGSSessionScreenIsLocked` is undocumented. An absent key fails admission explicitly; absence may be normal while unlocked. This can make macOS streaming unavailable and is an unresolved implementation contract, not merely an unrun acceptance test.
- Bounded primary-source review found no supported unprivileged general lock query in the searched Apple session/workspace APIs. This is not proof that none exists.
- Supported notification candidate: [EndpointSecurity loginwindow lock event](https://developer.apple.com/documentation/endpointsecurity/es_event_lw_session_lock_t), with corresponding unlock/login/logout events and graphical session identity. [Client creation requires root](https://developer.apple.com/documentation/endpointsecurity/es_new_client_result_err_not_privileged), an approved EndpointSecurity entitlement and user Full Disk Access; [system extension deployment](https://developer.apple.com/documentation/endpointsecurity/monitoring-system-events-with-endpoint-security) introduces signing/installation and privileged helper ownership. Notifications alone also require a trusted initial state, such as an observed unlock; helper startup cannot assume an unlocked session.
- Existing NSWorkspace session-deactivation notifications establish session switching, not a public screen-lock guarantee. Existing CGSession public keys do not establish the undocumented key's presence or semantics.
- No helper, new privilege, entitlement bypass or missing-key-as-unlocked behavior was added.

MANUAL STEP: Choose and authorize the macOS lock contract: an EndpointSecurity helper with Apple entitlement/signing/Full Disk Access and trusted initialization, or a specifically named macOS-version adapter whose lock-state behavior is independently validated and explicitly approved. Provide a matching macOS host for build and real lock/suspend/return acceptance before claiming macOS streaming operational support.

## Native evidence, 2026-09-21

Host: macOS26.5.1 /25F80 /arm64. Product lock contract remains unchanged.

Supported API review: local SDK `CGSession.h` documents on-console and login-complete, not screen-lock state. `Security/AuthSession.h` exposes graphical/TTY/remote/root attributes, not lock state. Apple's [CGSession dictionary](https://developer.apple.com/documentation/coregraphics/cgsessioncopycurrentdictionary()) and [NSWorkspace](https://developer.apple.com/documentation/appkit/nsworkspace) documentation do not establish a supported lock query. [EndpointSecurity client creation](https://developer.apple.com/documentation/endpointsecurity/client) requires root/entitlement/TCC. No supported unprivileged replacement identified in this bounded review; this is not proof none exists.

Probe: `.ltemp/native-20260921/lock-probe.m`, compiled against AppKit/ApplicationServices/Security without helper or entitlement. Samples every100ms; duration argument capped at600s; current runs bounded to90s. Records only session booleans/attribute bits and notification names, not usernames/content. Files are Git-excluded. Distributed lock notifications are observed diagnostics, not trusted authorization events.

| Observed state | Public on-console/login-complete | Private lock key | Other observations |
|---|---|---|---|
| Unlocked baseline | true/true | absent | SessionGetInfo succeeds; attributes0x6030 |
| Locked with Control–Command–Q | true/true | true | screenIsLocked notification; attributes0x2030 |
| Authenticated unlock | true/true | absent | screenIsUnlocked; attributes0x6030 |
| Actual system sleep | true/true | true after will-sleep | NSWorkspaceWillSleep; display asleep |
| Wake while locked | true/true | true | display awake |
| Authenticated unlock after wake | true/true | absent | unlock + NSWorkspaceDidWake; attributes still0x2030 |
| Process started while already locked | true/true | true | Fresh process observed locked for its entire15s run |
| Session switch and return | unmeasured | unmeasured | Requires another existing account; user availability requested |

Lock/unlock transition run contains902 samples/events. Sleep/wake contains590. Sampling paused during real sleep. Public on-console stays true while locked; SessionGetInfo's undocumented changing bits disagree with the lock state after wake and are unsuitable as an inferred supported unlock flag. Initial key absence alone still proves nothing under the public contract.

Observed product behavior: production worker acceptance test displays `This macOS login does not expose a verifiable lock state for streaming.` Installed source discovery instead stops earlier at missing Screen Recording permission; it did not reach this admission check in a captured installed-app stream. Keep these observations separate.

## Concrete private-adapter proposal, not implemented or approved

Proposed build-specific adapter in `MacSessionEnvironment.mm`:

1. Permit this private contract only for verified macOS26.5.1 build25F80 arm64. Reject unknown OS/builds and malformed/missing session dictionaries.
2. Require typed public on-console=true, login-complete=true and matching current-user identity. Inactive or changed sessions invalidate admission.
3. Start unknown on every application launch. An absent private key never initializes unlocked. Require a measured lock-key=true followed by an authenticated user unlock transition with the same on-console session and absent key before arming new offers. Poll the WindowServer dictionary; distributed unlock notifications alone must never authorize.
4. Poll every100ms. Key=true, sleep, session deactivation, query failure or the existing >3s processing gap stops media/control. Require a fresh validated unlock and a new visible offer after interruption; never resume an old stream.
5. Add separate adapter-policy tests without modifying protected suites. Before activation, complete session-switch/return native probes; reproduce installed sender/receiver Stop on lock and sleep after Screen Recording is authorized.

This deliberately requires lock/unlock calibration after application startup. It still relies on undocumented WindowServer absence-after-verified-unlock semantics and has up to100ms polling latency. It is not suitable as a hostile-local-process security boundary; distributed notification spoofing must not authorize unlock. OS updates fail closed until revalidated. Startup-locked was additionally observed in a fresh15-second process. These runs do not validate other accounts, all sleep paths or future builds.

APPROVAL REQUIRED: authorize this exact25F80 private-contract design and its startup lock/unlock requirement before product implementation. Native session-switch validation remains a prerequisite to enabling it. Alternative remains the explicitly privileged EndpointSecurity design above.

Bounded remaining probe: run `lock-probe 180` into a new `.ltemp` log; switch to another existing account without logging out; return and authenticate; verify on-console/session-loss evidence and fresh-state handling. For startup-locked, launch a separate bounded probe while the first reports key=true, then authenticate. Never create accounts or install helpers implicitly.
