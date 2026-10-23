# HomeGuardian AI — Testing Strategy

## Test Layers

### 1. Unit Tests
- Test individual classes and functions in isolation
- Use Catch2 framework
- Mock external dependencies via interfaces
- Fast execution (< 1 second per test)

### 2. Integration Tests
- Test interactions between components
- Use simulated sensors and events
- Verify end-to-end pipeline behavior
- Moderate execution time (< 10 seconds per test)

### 3. System Tests
- Test the full application with all components
- Use synthetic fixtures and simulated events
- Verify correct behavior under realistic conditions
- Longer execution time (seconds to minutes)

## Test Organization

```
tests/
├── unit/              # Unit tests per module
│   ├── core/
│   ├── sensors/
│   ├── inference/
│   ├── pipeline/
│   ├── persistence/
│   └── server/
├── integration/       # Integration tests
│   └── pipeline/
└── system/            # System tests
    └── full_app/
```

## Verification Evidence

Every test run produces:
- Test output (pass/fail per test)
- Execution time
- Coverage report (when available)

## Acceptance Requirements

A task is accepted only when:
1. All new unit tests pass
2. All existing tests still pass (no regressions)
3. Test output is captured and reviewed
4. Code coverage for new code is reasonable (> 70%)

## Failure Handling

- A failing test is never hidden or skipped
- Tests are never weakened to produce a green result
- If a test cannot pass, the task remains incomplete
- Failed tests are documented in `docs/STATUS.md`

## Running Tests

```bash
# All tests (with verbose output)
cd build && ctest --output-on-failure -V

# Specific test executable
cd build && ./tests/homeguardian_tests

# With Catch2 filter
cd build && ./tests/homeguardian_tests "[config]"

# With AddressSanitizer + UndefinedBehaviorSanitizer
cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && make -j4 && ctest --output-on-failure
```

## Test Coverage

Phase C adds tests for:
- Event construction, validation, JSON round-trip
- EventValidator: validation, normalization, provenance preservation
- SimulatedEventSource: replay order, exhaustion
- TimeWindowCorrelation: fires, insufficient events, outside window
- Pipeline: no alert, alert generation, history bounds
- Alert: creation, JSON round-trip
- Config: new fields (max_event_history, correlation_window_ms, alert_confidence_threshold)

## Test Fixtures

- Synthetic events: generated programmatically with known properties
- Simulated sensors: produce deterministic output for reproducible tests
- Mock AI providers: return predefined results
- Test data: stored in `tests/fixtures/`

## Continuous Testing

After every commit:
1. Build the project
2. Run all tests
3. Review test output
4. Only commit if all tests pass

## D-03 production ingress (real mutual TLS)

The optional TLS ingress is compiled only when OpenSSL is available. Its cases are
tagged `[.]` because they need generated certificates, so they run through the
registered ctest rather than the default suite:

```
cmake --build hg_build --target homeguardian_tests hg_device_ingress -j2
ctest --test-dir hg_build -R production_ingress_tls --output-on-failure
python3 tests/net/run_production_ingress.py        # 4 cases, 325 assertions
python3 tests/net/run_ingress_binary_smoke.py      # shipped binary, real handshake
```

These generate a throwaway CA and client/server certificates inside `hg_build` and
bind an ephemeral loopback port. They prove the production wiring path on the
workstation; they do not install, deploy or exercise a phone, and a pass here is not
physical or alert acceptance. Note that the active native executable is
`hg_build/homeguardian_tests` — older `build/` paths above are historical.
