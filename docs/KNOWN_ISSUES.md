# Known issues

## Development Secure privileged input is not physically qualified

The explicit privileged-input option is confined to the privately trusted
Development Secure package and remains experimental until the signed two-PC
matrix passes. DeskLink does not stream the secure desktop, so the person
authorizing a UAC prompt must already be able to see the controlled PC's
display. On `Winlogon`, DeskLink intentionally rejects keyboard and complete
input-state reconciliation; only pointer motion, buttons, wheel, and release
are accepted while `consent.exe` is foreground. This permits a manual visible
Yes/No decision without enabling remote administrator-secret entry. Lock and
sign-in desktops remain unsupported.

## External virtual-audio cable application input is not physically qualified

DeskLink can now route received voice through a replaceable application-output
backend to one exact, user-selected WASAPI render endpoint. VB-CABLE is the
first documented compatibility target, but its physical two-PC Discord/OBS,
latency, revoke, disconnect, endpoint-removal, and restart matrix has not yet
been completed. DeskLink does not bundle or silently install VB-CABLE. After a
user installs it, DeskLink must select `CABLE Input` while the receiving
application selects `CABLE Output`. A missing exact endpoint fails closed and
does not fall back to speakers or another provider.

## Unicode punctuation renders as mojibake in the current Windows alpha

The Windows alpha artifact built from commit `a45e963` may show sequences such
as `â€”`, `Ã—`, or `Â·` where the interface intends to display an em dash,
multiplication sign, or middle dot. This is a presentation defect: it does not
alter peer identities, monitor dimensions, transport data, or saved roaming
configuration.

The source now forces UTF-8 source and execution character sets for MSVC builds.
Install a Windows artifact containing that fix when one is published. The
affected artifact has no runtime setting that can reliably correct the text.
