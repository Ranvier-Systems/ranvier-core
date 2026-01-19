1. Ref docs/claude-context.md for project constraints.
2. Context: We recently identified issues in our Adversarial Audit or implementation review.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

I am requesting a POST-MORTEM PATTERN EXTRACTION.

**Issues to formalize:**
- <ISSUE_1>
- <ISSUE_2>
- <ISSUE_3>

---

## PERSONA

Act as a **Principal Engineer** performing a Post-Mortem. Transform recent bug fixes and audit findings into formal "Anti-Patterns & Lessons Learned" entries for `claude-context.md`.

---

## WHEN TO USE THIS PROMPT

**Mandatory after:**
- Any adversarial audit with findings rated 4+/10
- Any production bug caused by code patterns
- Any significant refactoring that revealed hidden issues

**Optional after:**
- Any feature implementation (even if no issues found—document what worked)

---

## ENTRY FORMAT

For each major issue, structure the entry as follows:

```markdown
#### N. The [Descriptive-Name] Anti-Pattern

**THE PATTERN:** What was the "seemingly good" but incorrect approach used?

**THE CONSEQUENCE:** Why is this dangerous in our Seastar/12k LOC environment? Be specific about failure modes.

**THE LESSON:** *Hard Rule: [One sentence imperative rule].* Provide implementation guidance.

**PROMPT GUARD:** "[One-sentence instruction to add to future prompts to prevent this pattern.]"
```

---

## QUALITY CRITERIA

Each entry must:
1. **Be specific** - Reference actual code patterns, not abstract concepts
2. **Explain the "why"** - Why does this fail in Seastar specifically?
3. **Be actionable** - The Hard Rule must be immediately applicable
4. **Be preventable** - The Prompt Guard must be copy-pasteable into prompts

---

## OUTPUT FORMAT

### New Anti-Patterns (for claude-context.md)

```markdown
---

#### N. The [Name] Anti-Pattern

**THE PATTERN:** ...

**THE CONSEQUENCE:** ...

**THE LESSON:** *Hard Rule: ...*

**PROMPT GUARD:** "..."

---
```

### Quick Reference Update

If adding new rules, also provide the row for the Quick Reference table:

```markdown
| N | [Short rule] | [Violation description] |
```

### Prompt Guard Collection

List all new Prompt Guards for easy copy-paste into other prompts:

```
- "Never [do X]—always [do Y] because [reason]."
- "..."
```

