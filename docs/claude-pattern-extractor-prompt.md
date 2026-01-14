1. Ref docs/claude-context.md for project constraints.
2. Context: We recently identified issues in our Adversarial Audit or implementation review.

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

---

## Extracted Anti-Patterns (2026-01-14 Audit)

The following anti-patterns were extracted from the adversarial audit conducted on 2026-01-14.

### New Anti-Patterns (for claude-context.md)

---

#### 12. The Blocking-ifstream-in-Coroutine Anti-Pattern

**THE PATTERN:** Using `std::ifstream` or `std::ofstream` inside a coroutine or Seastar method: `std::ifstream file(path); buffer << file.rdbuf();`

**THE CONSEQUENCE:** `std::ifstream` performs blocking I/O. In Seastar, this stalls the reactor thread—stopping all network I/O, timer callbacks, and request processing on that shard. A 10ms disk read becomes 10ms of zero throughput.

**THE LESSON:** *Hard Rule: Use Seastar file I/O APIs.* Use `seastar::open_file_dma()` + `seastar::make_file_input_stream()` for async file reads. For small files during startup only, document the blocking nature explicitly.

**PROMPT GUARD:** "Never use std::ifstream/ofstream in Seastar code—use seastar::open_file_dma and seastar::make_file_input_stream for async file I/O."

---

#### 13. The Thread-Local-Raw-New Anti-Pattern

**THE PATTERN:** Using `thread_local T* g_ptr = nullptr;` with `g_ptr = new T();` for per-shard state.

**THE CONSEQUENCE:** No corresponding `delete` call exists. Thread-local variables aren't destroyed by unique_ptr RAII. Memory leaks accumulate over the process lifetime. Tools like valgrind report leaks at exit.

**THE LESSON:** *Hard Rule: Use `thread_local std::unique_ptr<T>` or add explicit destroy function.* Alternatively, use Seastar's `seastar::sharded<T>` service pattern which handles per-shard lifecycle correctly.

**PROMPT GUARD:** "Never use raw 'new' with thread_local pointers—use unique_ptr or add an explicit destroy/cleanup function called during shutdown."

---

#### 14. The Unbounded-Per-Entity-Map Anti-Pattern

**THE PATTERN:** Using `std::unordered_map<EntityId, State>` where entities come from external input (backend IDs, peer addresses, request IDs) without size limits.

**THE CONSEQUENCE:** Adversarial input can create unlimited entities. Each map entry consumes memory. Under attack, memory grows until OOM. Production: 10 backends = fine. Attack: 10 million spoofed IDs = crash.

**THE LESSON:** *Hard Rule: Every per-entity map needs MAX_SIZE + eviction.* Either tie to entity lifecycle (remove when entity removed) or add LRU eviction + overflow counter metric.

**PROMPT GUARD:** "Every map keyed by external IDs must have explicit MAX_SIZE with eviction strategy—LRU, oldest-first, or tied to entity lifecycle."

---

### Quick Reference Update

| # | Rule | Violation |
|---|------|-----------|
| 12 | Use Seastar async file I/O | `std::ifstream`/`std::ofstream` in Seastar code |
| 13 | No raw `new` with `thread_local` | `thread_local T* p; p = new T();` without delete |
| 14 | Cap per-entity maps | `unordered_map<ExternalId, State>` without MAX_SIZE |

### Prompt Guard Collection

```
- "Never use std::ifstream/ofstream in Seastar code—use seastar::open_file_dma and seastar::make_file_input_stream for async file I/O."
- "Never use raw 'new' with thread_local pointers—use unique_ptr or add an explicit destroy/cleanup function called during shutdown."
- "Every map keyed by external IDs must have explicit MAX_SIZE with eviction strategy—LRU, oldest-first, or tied to entity lifecycle."
```

