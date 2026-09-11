# Left modifier swap — Mac verification

Status: blocked-manual

- Implemented and enabled on the Windows server for `Fakhreddines-MacBook-Pro.local`.
- Rebuilt/restarted Deskflow. Full build and 32-test quick check passed. New suite: 46 Qt Test passes including setup/cleanup. Check-test: 72 individually injected assertion failures followed by restored passes; all temporary faults removed.
- Actual build/test/runtime logs: `temp/proof-of-work/left-modifier-swap/`. Blind proof check passed for the final test/build/runtime logs and a fault/restoration pair.
- No UI layout or styling was changed. Mac-side screenshot proof of the keyboard behavior is still required by the UX proof rule; this session has no Mac screen capture access. Do not mark final visual verification complete without it.
- Existing game exclusion remains active: sharing pauses while `bf6.exe` is foreground.

MANUAL STEP: On the Mac, capture Keyboard Viewer showing Command highlighted while the PC's left Ctrl is held, then Control highlighted while the PC's left Windows key is held. Save the screenshots under `temp/proof-of-work/left-modifier-swap/` and blind-check them before marking this task done.

Also verify Ctrl+C/V on the PC keyboard invokes Command+C/V on the Mac, right Ctrl stays Control, and local Windows shortcuts remain unchanged. Close this task and remove its PROJECT.md index entry after verification.
