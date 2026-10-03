# Version and display diagnostics

The shell, broker, and input runtime embed the build label from
`include/desklink/build_version.h` as a separate Windows resource. Bump this label
for each shipped build. Installer file-version stamping does not replace it.

The shell shows a warning across every page if the broker or active input
runtime has a different label, or an older executable has no label. Diagnostics
shows each component's label, the shell's control and PC connection protocol
versions, and the executable paths behind the admitted current-user pipes.
An unavailable pipe is reported as unavailable, not as a proven version mismatch.
The labels describe the executable at the running process's path; they are
diagnostic metadata and never authorize a process, peer, capability, or route.

The pipe client's existing user-identity and pipe-security checks run before
reading metadata. Executables are opened as resource data without executing
their code. The control and network wire formats are unchanged. Peer release
labels are not exchanged; this warning compares local components, while the
existing discovery and handshake checks enforce PC connection protocol versions.

`desklink_pair control versions` prints local client, broker, and input runtime
build labels and paths. `desklink_pair control topologies` prints each machine's
actual topology status and active/connected display counts. Neither command
changes pairing, saved links, focus, or routing.

The display refresh banner reports success only when all paired PCs have ready
topologies. A connected transport with missing or rejected monitor data produces
a warning. Current connected-display inventories prune obsolete canvas tiles;
saved explicit routes retain their existing validation and identity checks.
