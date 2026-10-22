# Animal activity and environmental precursors — research boundaries

## Two categories, never merged

**Empirically validated (testable in this codebase today)**

* Motion and audio analysis: change detection, level/trend features, presence/occupancy patterns.
* Anything reproducible by the engine with a synthetic fixture and an honest negative control.
* A statement these support: "audio evidence changed at this window with this confidence."
* A statement these do NOT support: any claim about the *cause* of the change.

**Unvalidated / speculative (hypotheses only — never described as capability)**

* Species inference, animal-vs-animal discrimination, insect/rodent/bird identification from cheap sensors.
* "Animals behave unusually before earthquakes/storms/disasters" and similar folklore. This is a hypothesis
  with, at present, no validated evidence base in this project.
* No causal disaster-warning claim may be made, and no roadmap item may be phrased as if it already works.

## What any investigation must account for

* privacy and consent: **no continuous production capture without explicit approval**;
* false positives and the base rate (a rare event needs a very low false-positive rate to be useful);
* sensor noise, illumination changes, and baseline drift over days (ordinary ambient motion and lighting
  changes have already triggered an optional activity episode detector — treat this as an open problem);
* ordinary household behaviour as the **null hypothesis** every time;
* species uncertainty — "something moved" is not "a fox moved";
* sensor placement and coverage limits;
* whose data it is, where it is stored, and who can read the MCP projection.

## Reporting rule

State absence of evidence as absence of evidence. If a pattern is found, report the pattern, the window, the
confidence and the negative control — not a story about what the pattern means. Physical camera/microphone
activity is opt-in per task and is never implied by this skill.
