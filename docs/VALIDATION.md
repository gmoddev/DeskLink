# DeskLink Foundation Validation

## Validation environment

The portable core was built in the artifact-generation environment using:

```text
GCC 14.2.0
CMake 3.31.6
C++20
```

The Windows-only `desklink_windows` target was disabled because the validation environment is Linux. Its source is included for Windows builds.

---

## Normal build

Commands:

```bash
cmake -S . -B build -DDESKLINK_BUILD_WINDOWS_BACKENDS=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed
```

---

## Sanitizer build

The portable core/tests were also rebuilt with:

```text
-fsanitize=address,undefined
-fno-omit-frame-pointer
```

Result:

```text
100% tests passed, 0 tests failed
```

No AddressSanitizer or UndefinedBehaviorSanitizer failure was reported by the test run.

---

## Independent remote revalidation

The public repository was cloned and rebuilt on 2026-08-21 using Docker Desktop
on a separate Windows PC. The source was mounted read-only into disposable Linux
containers.

Release validation passed with both toolchains:

| Toolchain | CMake | Result |
|---|---:|---|
| GCC 15.2.0 | 4.2.3 | Build, tests, and simulation passed |
| Clang 22.1.3 | 4.2.3 | Build and tests passed |

The GCC build was repeated with AddressSanitizer and UndefinedBehaviorSanitizer.
All tests passed with no sanitizer failure reported.

The remote Windows host did not have CMake or the Visual Studio C++ workload
installed. GitHub Actions subsequently built and tested the project with MSVC
on `windows-latest`, including compilation of the native `desklink_windows`
target.

---

## Simulation output

The simulation demonstrates:

- focus acquisition
- epoch creation
- keyboard event acceptance
- absolute pointer acceptance
- key release
- lease expiry cleanup
- stale packet rejection
- Host emergency fail-local

Representative output:

```text
DeskLink foundation simulation
requesting remote focus...
agent decision=0
focus epoch=2
agent: key scan=30 down
agent decision=0
agent: pointer display=0 x=42000 y=25000
agent decision=0
agent: key scan=30 up
agent decision=0
simulating network silence past lease...
agent: release DeskLink-owned key/button state
stale decision=3
host mode after emergency=1
```

---

## Physical pairing-control check

On two Windows 11 or Windows Server 2022-or-newer PCs on the same trusted LAN,
build `desklink_pair` against the pinned Schannel package. Start the listener on
the PC that should accept remote input:

```powershell
desklink_pair.exe listen 43821 --grant-input
```

Then connect from the other PC:

```powershell
desklink_pair.exe pair <listener-ip> 43821
```

Verify that both prompts show the same six-digit code and the intended
capability consequence before selecting Yes. Repeat after restarting both tools
to verify that `%LOCALAPPDATA%\DeskLink\trust.db` remains readable. A firewall
exception, if needed, must be created manually and limited to the appropriate
Private or Domain network profile.

For SSH-driven compatibility validation where Windows session isolation hides
message boxes, add `--console-confirm` to both pairing commands. Each side prints
the same six-digit code and waits for the exact input `yes`; no confirmation is
automatic. A local confirmation is exchanged on the peer-validated pairing
stream, and neither side reports success or persists new trust until both
confirmations arrive. Trusted connections also log their shared session nonce so
reconnect rotation can be recorded without exposing identity or trust secrets.

Windows 10 remains unsupported. The MsQuic 2.6.x foundation, opaque CNG/OpenSSL
provider, fail-closed admission matrix, identity-invariance comparison, and
private-key-export source gates now pass. It is now eligible for the guarded
physical compatibility-validation matrix, but remains unsupported until that
matrix passes. It never receives an OpenSSL reduced-security fallback.

The guarded physical Windows 11 Schannel to Windows 10 OpenSSL run has passed
manual same-code pairing, bilateral trust persistence, trusted reconnect, two
matching cross-provider session nonces, nonce rotation on the second reconnect,
and focus without capture. The before/after identity snapshots were identical
on both PCs and retained export policy zero. Physical capture and failure tests
below remain incomplete, so this is not a Windows 10 support claim.

The native MsQuic loopback additionally verifies that both trusted endpoints
receive the same nonzero session nonce and that reconnecting rotates it.

Stage 2 CI builds the pinned OpenSSL 3.5.7 source, applies the reviewable patch
to the exact MsQuic 2.6.0 commit, and runs both provider loopbacks. Its OpenSSL
gate verifies pairing, trusted reconnect, fresh nonce, reliable traffic,
exportable-key rejection, certificate/key mismatch rejection, and pinned hash
rejection for MsQuic, libcrypto, and libssl. It also rejects the patch if it
mentions private-key export APIs, ENGINE, or `RSA_METHOD`. These are prototype
gates. Stage 3 adds missing/malformed/time-invalid DER classification, wrong
pin, unknown peer, validator failure/exception/timeout, signing failure, and
changed-identity rejection. Every network case asserts that no pairing or
trusted application session was delivered.

Stage 4 snapshots both loopback identities before and after Schannel and
OpenSSL/CNG handshakes. It compares the actual CNG key name, provider,
algorithm, export policy, certificate public-key DER, certificate DER hash, and
DeskLink identity pin byte-for-byte. A CMake build target and test reject the
three prohibited private-key export APIs across the DeskLink credential/runtime
boundary and downstream patch.

For a physical-test record, run `desklink_pair.exe identity` on each PC before
and after the matrix and preserve the output. The command emits the full public
SubjectPublicKeyInfo DER plus every other invariant field; its two records must
match exactly, and it exits unsuccessfully if the export policy is nonzero.

After pairing with `--grant-input` on the receiving PC, run
`desklink_pair.exe serve 43821` there and
`desklink_pair.exe focus <receiver-ip> 43821 --capture` on the other PC. Verify
that keyboard, buttons, and pointer movement reach the receiver while the sender
stays locally suppressed. Press Ctrl+Alt+Pause and verify local input returns
immediately, the remote lease is released, and held remote state is cleaned up.
Repeat using Enter as the normal release path.

---

## Limitations of this validation

This validation does not yet prove:

- sustained high-poll-rate keyboard-hook/Raw Input timing across physical Windows 11 PCs
- WASAPI timing or endpoint recovery
- real packet-loss/jitter characteristics
- physical input-state snapshot recovery after an interrupted key/button transition

The Windows CI job additionally runs a native MsQuic 2.6.0 Schannel loopback:
two current-user CNG identities exchange bounded offers, confirm the same code,
reconnect with mutual pins, and transfer a reliable DeskLink packet. The test
deletes both temporary identities on success or failure.

Those are explicitly production integration stages rather than silently assumed complete features.
