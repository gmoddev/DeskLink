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

## Limitations of this validation

This validation does not prove:

- Windows `SendInput` source compiles on every supported MSVC/SDK combination
- MsQuic behavior, because the MsQuic adapter is not yet implemented
- Raw Input/hook timing behavior
- WASAPI timing or endpoint recovery
- real packet-loss/jitter characteristics
- pairing cryptography

Those are explicitly production integration stages rather than silently assumed complete features.
