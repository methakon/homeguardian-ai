# HomeGuardian AI — Implementation Workflow

## Overview

All code is implemented directly with full architectural review.
No AI coding agents are used for implementation.

## Responsibilities

| Responsibility | Owner |
|---------------|-------|
| Architecture decisions | Developer |
| Coding and implementation | Developer |
| Builds and tests | Developer |
| Git operations | Developer |
| Code review | Developer |
| Documentation | Developer |

## Workflow

1. Design interfaces and architecture before coding
2. Implement small, focused modules
3. Build and test after each module
4. Review diff before commit
5. Commit only when all tests pass

## Build and Test

```bash
mkdir build && cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DHOMEGUARDIAN_BUILD_TESTS=ON ..
make -j4
ctest --output-on-failure -V
```

## Git Rules

- Keep commits focused on one logical change
- Never commit failing tests
- No force-push, no history rewrite
- Review diff before every commit
