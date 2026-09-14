# Item 09 — Wayland physical-priority dependency

Status: product decision pending; no relaxation implemented.

## Verified API boundary

- [RemoteDesktop portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html): `ConnectToEIS` supplies a **sender** context; combined ScreenCast/RemoteDesktop consent can bind capture and input to one portal session.
- [libei client API](https://libinput.pages.freedesktop.org/libei/api/group__libei.html): pointer, button and keyboard events are generated only on a **receiver** context. Sender contexts receive device lifecycle and modifier state; modifier notifications do not identify physical input. A context cannot be both sender and receiver. Device pause resets held state but is not specified to occur for every physical action.
- [InputCapture portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.InputCapture.html): only the compositor can activate capture. `Enable` merely arms it; screen-edge pointer barriers are a defined trigger and `Activated` also permits unspecified other triggers. The API does not guarantee application-requested immediate activation. Therefore it cannot guarantee an always-on local physical-input observer during a streaming lease.

Inference: the standard portal APIs alone do not meet this worklist's requirement to revoke on **any local physical input before core processing/forwarding and further control injection**. Treating EIS device pause, modifier changes, portal Stop, or a screen-edge barrier as equivalent would weaken that requirement. ScreenCast permission never authorizes input.

## Reviewable choices

1. Preserve the priority contract and keep Wayland streaming explicitly view-only in this worklist. RemoteDesktop/EIS input remains blocked implementation until a suitable platform mechanism is selected; this does not claim complete Wayland interactive support.
2. Extend scope to a specifically selected compositor/version with a compositor-side authenticated physical-input revocation integration. That integration must distinguish physical input from this session's EIS events, revoke/neutralize EIS devices before processing physical input, bind exact portal session/source, and expose an authenticated session-specific signal to the core. This needs a named target compositor plus a host for its extension/plugin build and acceptance. Generic EIS cannot substitute for the integration.

Do not add privileged `/dev/input` access, global grabs, synthetic reinjection of intercepted local events, compositor extensions, or altered physical-priority semantics without the corresponding explicit product choice.

MANUAL STEP: choose continued view-only Wayland scope or name the compositor/version and authorize its physical-priority integration. macOS/X11/Windows work continues independently.
