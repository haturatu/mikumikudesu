# PR6 fixture oracle (skeleton)

Windows 1.30 fixtures are the oracle for solver/physics/IO parity.
This document records the acceptance contract; binaries land out of tree.

## Fixtures (planned)

| fixture | oracle | tolerance | status |
| --- | --- | --- | --- |
| `camera-parent.vmdayo` | parentModel/parentBone/parentBoneName resolve + missing fallback | exact match, missing -> identity | planned |
| `catmull-axis.vmdayo` | bone 4-axis + camera 6-axis method routing | bezier within `SolverCompatibilityProfile::cameraTolerance` | planned |
| `multi-model.dayo` | camera/light subset, dictionary, metadata, all tracks | exact subset names, key counts | planned |
| `external-parent.dayo` | deleted parent, unresolved parent, cycle rejection | `Scene::addExternalParent` error strings | planned |
| `gravity.dayo` | gravity keys + saved settings round trip | strength 1e-3, direction normalized | planned |
| `limited-ik.pmx` + `limited-ik.vmdayo` | 1/2/3-axis + knee/asymmetric limits, same-frame bone matrix + endpoint | position 1e-3, rotation 1e-4 | planned |

## Procedure

1. Native save is never presented as an upstream fixture.
2. Upstream save -> native load -> native serialize -> native reload checks
   unknown-payload preservation + the oracle column above.
3. Native save -> upstream load confirms forward interop on Windows hardware.
4. Record upstream Release, operation steps, model provenance/redistribution
   terms, and execution environment with every new fixture.

## Harness

- `tools/capability_suite --list` enumerates suites.
- `MotionSolver::compareBones/compareCamera` implement tolerance checks.
- `PhysicsStepper` + `PhysicsCompatibilityProfile` pin the fixed-step path.
- Sequence/HDR oracles live in `tests/editor_physics_io_tests.cpp`.
