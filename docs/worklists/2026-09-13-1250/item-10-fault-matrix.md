# Item 10 fault matrix

All execution uses owned fixtures and isolated IPC. Native active-desktop, cross-host and remote-OS acceptance remains separately gated.

| Fault / lifecycle | Evidence / implementation | Current state |
|---|---|---|
| Retired queued media error | red-01; transport identity guard; real launcher before/after | fixed; insurance/visual blind PASS |
| Stop before slow cleanup | Stop emitted before native cleanup; destructor closes private IPC before worker join | fixed; precise order/exit insurance blind PASS |
| Native capture startup | Initial Stopped callback guarded; distinct Starting signal/poll/name | fixed; red/green/insurance PASS; native source loss acceptance pending |
| Future audio epoch | First future PCM retained; old output paused; stale blocks discarded; three-second deadline | fixed; six policy insurance cases PASS |
| Queued GUI actions | Session tickets for Stop/Accept/volume/playback/grant/revoke/focus; input lease binding; retained pending Start | fixed; independent insurance PASS |
| Backend privacy | Actual audio/media/capture GStreamer bus diagnostics; rendered sanitized errors | fixed; red/green/insurance and all image pairs blind PASS |
| Lock/suspend | Windows message-only WTS notifications and power callback plus active/unlocked query; streaming worker boundary | local boundary/owned-message tests PASS; actual OS transitions blocked; Mac lock API unresolved |
| Windows service IPC | Exact-logon native pipe, restricted delegated rights, reciprocal token/session checks, identification-only client | current-login/policy insurance PASS; privileged cross-token runtime approval blocked |
| Bilateral Stop / receiver exit / reconnect | One controller and worker reuse12 real file/PCM sessions through broker/private IPC/separate DTLS receiver; fresh offer/consent per cycle | revised insurance-08 and final unique-consent/resource log blind PASS; actual cross-host exit remains pending |
| Resource cleanup | Required successful native memory/handle/thread/UDP queries at each phase; retained controller; post-Stop media-owner assertion | insurance-08 actual resources retained; old05 has narrower Stop-only scope;06/07 fixture setup failures excluded |
| Control release | Existing Service retry retains arbitration after failed native release; protected lifecycle/transport suites; GUI-exit IPC detachment | earlier protected transport10/control15 PASS; final full pending; native physical release blocked |
| Input/clipboard coexistence | Unchanged customization/clipboard/input suites during real separate-process media load | coexistence-01 ordinary suites pass during15 real media cycles; hidden quick48/50; native routing acceptance blocked |
| Media transient | resources-03 media-full-1 passes23; full2/full3 fail only overload with partial decoded startup; both endpoints clean up | overload regression unresolved; no causal claim for initial09 failure |

Insurance-audit-manifest-01 references60 counted baseline/fault/restoration executions. Exclude insurance-01 case03 duplicate cleanup, insurance-02 case32 superseded AccessCheck mapping and case33 ineffective GA mutation. Corrected AccessCheck expands generic ACE masks before checking individual read/write and pipe-instance rights. Supplemental persistent cleanup insurance is replaced by the latest revised fixture run for current-state accounting.

Overload investigation: a temporary keyframe maximum250% to100% trial reduced measured worst raw keyframes42KB to28KB but still failed with partial decoded startup. Original250% restored; diagnostic probes removed. Existing GCC allocation subtracts Opus payload bitrate but does not reserve authenticated RTP metadata overhead. No arbitrary margin or protected-test change was introduced. Whether production congestion recovery or the protected fixture's accepted outcomes require change remains unresolved.

Final: ON/OFF explicit builds pass; full52/54 in80.25s and initial final quick48/50 in22.07s fail foreground and protected raw-audio contract. Exact test update awaits approval. Post-documentation final quick uses quick-check-final-03.txt. Item10 is blocked-approval; next work is orchestrated item11 and batched manual actions. No actual user lock/suspend/input, daemon launch or elevated token acquisition was performed.
