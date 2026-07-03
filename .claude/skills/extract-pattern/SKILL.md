---
name: extract-pattern
description: Formalize a bug, audit finding, or incident root cause into a numbered Hard Rule in claude-context.md. Use after fixing a bug caused by a repeatable code pattern, after audits with findings rated 4+/10, or when the user says "make this a rule".
argument-hint: [issues-to-formalize]
---

I am requesting a POST-MORTEM PATTERN EXTRACTION.

**Issues to formalize:**
$ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## PERSONA

Act as a **Principal Engineer** performing a Post-Mortem. Transform recent bug fixes and audit findings into formal "Anti-Patterns & Lessons Learned" entries.

## WHEN TO USE

**Mandatory after:**
- Any adversarial audit with findings rated 4+/10
- Any production bug caused by code patterns
- Any significant refactoring that revealed hidden issues

**Threshold check first:** a Hard Rule is for *systemic* patterns — mistakes that will plausibly recur in new code. A one-off bug gets a test, not a rule. A Seastar subtlety that is real but hasn't bitten us yet goes in `.dev-context/seastar-pitfalls-reference.md` instead (the "minor leagues" — promote it later if it bites).

## QUALITY CRITERIA

Each entry must:
1. **Be specific** - Reference actual code patterns, not abstract concepts
2. **Explain the "why"** - Why does this fail in Seastar specifically?
3. **Be actionable** - The Hard Rule must be immediately applicable
4. **Be preventable** - The Prompt Guard must be copy-pasteable

## ENTRY FORMAT

```markdown
---

#### N. The [Descriptive-Name] Anti-Pattern

**THE PATTERN:** What was the "seemingly good" but incorrect approach used?

**THE CONSEQUENCE:** Why is this dangerous in our Seastar/shared-nothing environment? Be specific about failure modes.

**THE LESSON:** *Hard Rule: [One sentence imperative rule].* Provide implementation guidance with correct/incorrect code examples.

**PROMPT GUARD:** "[One-sentence instruction to prevent this pattern.]"

---
```

Take the next free number (rules are append-only; never renumber).

## PROPAGATION CHECKLIST — a rule that lives in one file only is not a rule

When adding Hard Rule #N, update **all** of:
- [ ] `.dev-context/claude-context.md` — full anti-pattern entry (format above)
- [ ] `.dev-context/claude-context.md` — Quick Reference table row at the bottom
- [ ] `.dev-context/claude-context.md` — Pre-Code Checklist item at the top, if the rule is checkable before writing code
- [ ] `.claude/skills/review/SKILL.md` — new row in the Compliance Table
- [ ] `.claude/skills/debug-build/SKILL.md` — symptom row, if the rule has a recognizable failure signature
- [ ] Existing violations in `src/`? grep for the pattern; file `[TECH-DEBT]` BACKLOG items for any found (don't silently ignore them)

## CORRECTING AN EXISTING RULE

If the finding shows an existing rule is *wrong or incomplete* (it has happened — see the `*Corrected 2026-05-05*` notes on Rules #2, #12, #13, #20): edit the rule in place, keep the number, and append a dated `*Corrected YYYY-MM-DD: [what changed and why]*` line. Never delete the correction trail.

---

## OUTPUT FORMAT

1. The full anti-pattern entry (ready to append)
2. The Quick Reference table row
3. The `/review` compliance-table row
4. The list of files updated (propagation checklist, all boxes checked)
5. Prompt Guard collection for easy copy-paste
