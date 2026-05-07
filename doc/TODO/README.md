# ProvSQL TODO

Planning material for upcoming ProvSQL work, kept alongside the source
tree so the plans evolve with the code that implements them.

Each plan document follows a consistent layout:

1. **Intro** : one paragraph stating the scope of the plan and the
   reference material it is anchored on.
2. **Out of scope** (optional) : items deliberately excluded, with a
   pointer to where they are handled instead.
3. **Plan** : the proposals themselves, each self-contained.
4. **Priorities** : ship-when ordering.
5. **Implementation observations** (optional) : reusable notes from
   prior work in the same area.

## Contents

- [`case-studies.md`](case-studies.md) : plan for closing the
  feature-coverage gaps in the user tutorial and the five existing
  case studies (CS1-CS5), plus a sketch of CS6 for upcoming features.
- [`compiled-semirings.md`](compiled-semirings.md) : plan for new
  compiled semirings under `src/semiring/`, anchored on the Lean
  formalisation from [https://provsql.org/lean-docs/]
- [`feature-coverage.md`](feature-coverage.md) : reference matrix
  cross-referencing every documented user-facing feature against the
  tutorial and case studies. Source of truth for the gaps that
  `case-studies.md` proposes to close.
