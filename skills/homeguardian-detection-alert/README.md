# homeguardian-detection-alert — install and use

## Verified discovery path (checked, not assumed)

Hermes on this machine loads skills from **`~/.hermes/skills/<category>/<name>/SKILL.md`**. That was verified by
inspection, not assumption: the pre-existing `homeguardian-ai` skill resolves to
`~/.hermes/skills/software-development/homeguardian-ai/SKILL.md`, and `~/.hermes/profiles/default/skills/` is
empty. There is **no project-auto-discovery of skills from inside a git checkout** in this setup — a skill that
exists only in the repository is versioned, not loaded.

So there are two locations, with different jobs:

| location | job |
|---|---|
| `skills/homeguardian-detection-alert/` (this repo, in the branch) | versioned source of truth — reviewed, diffable, pushed |
| `~/.hermes/skills/software-development/homeguardian-detection-alert/` | what the loader actually reads |

## Install (local, one command)

```bash
ln -s "$(pwd)/skills/homeguardian-detection-alert" \
      ~/.hermes/skills/software-development/homeguardian-detection-alert
```

A symlink (not a copy) keeps the repo as the single source, so a `git pull` updates the loaded skill. After
installing, confirm the loader sees it by asking Hermes to view the skill; if it does not resolve, the symlink
target is wrong — do not report success from the filesystem alone.

## Use

Load the skill before detection/alert work, then read the code it points at. Run the checks:

```bash
bash skills/homeguardian-detection-alert/scripts/verify_skill.sh          # lint + anchor re-check
bash scripts/verify_fusion_window.sh                                      # the positive/negative fixture
```

`verify_skill.sh` asserts that every `file:line` the skill quotes still exists in this checkout, that the
frontmatter and required sections are present, that the example frames parse — and that the invalid example is
rejected for the reason claimed. If the engine moves, the lint fails instead of the skill quietly lying.

## Uninstall

```bash
rm ~/.hermes/skills/software-development/homeguardian-detection-alert   # removes the symlink only
```
