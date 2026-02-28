I am requesting a POST-MORTEM PATTERN EXTRACTION.

**Issues to formalize:**
- <ISSUE_1>
- <ISSUE_2>
- <ISSUE_3>

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the 24 Hard Rules.

---

## PERSONA

Act as a **Principal Engineer** performing a Post-Mortem. Transform recent bug fixes and audit findings into formal "Anti-Patterns & Lessons Learned" entries for `.dev-context/claude-context.md`.

## WHEN TO USE THIS PROMPT

**Mandatory after:**
- Any adversarial audit with findings rated 4+/10
- Any production bug caused by code patterns
- Any significant refactoring that revealed hidden issues

---

## ENTRY FORMAT

For each major issue:

```markdown
#### N. The [Descriptive-Name] Anti-Pattern

**THE PATTERN:** What was the "seemingly good" but incorrect approach used?

**THE CONSEQUENCE:** Why is this dangerous in our Seastar/shared-nothing environment? Be specific about failure modes.

**THE LESSON:** *Hard Rule: [One sentence imperative rule].* Provide implementation guidance.

**PROMPT GUARD:** "[One-sentence instruction to prevent this pattern.]"
```

## QUALITY CRITERIA

Each entry must:
1. **Be specific** - Reference actual code patterns, not abstract concepts
2. **Explain the "why"** - Why does this fail in Seastar specifically?
3. **Be actionable** - The Hard Rule must be immediately applicable
4. **Be preventable** - The Prompt Guard must be copy-pasteable

---

## OUTPUT FORMAT

### New Anti-Patterns (for .dev-context/claude-context.md)

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

Row for the Quick Reference table:
```markdown
| N | [Short rule] | [Violation description] |
```

### Prompt Guard Collection

All new Prompt Guards for easy copy-paste:
```
- "Never [do X]--always [do Y] because [reason]."
```
