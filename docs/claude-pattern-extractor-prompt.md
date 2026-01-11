1. Ref docs/claude-context.md for project constraints.
2. Context: We recently identified several issues in our Adversarial Audit (High/Medium/Low).

TASK: Act as a Principal Engineer performing a Post-Mortem. Transform our recent bug fixes and audit findings into a formal "Anti-Patterns & Lessons Learned" entry.

For each major issue found, structure the entry as follows:
- THE PATTERN: What was the "seemingly good" but incorrect approach used?
- THE CONSEQUENCE: Why is this dangerous in our Seastar/12k LOC environment?
- THE LESSON: What is the specific 'Hard Rule' to prevent this?
- PROMPT GUARD: Provide a 1-sentence instruction I can add to future prompts to ensure you never do this again.

OUTPUT: Provide a Markdown-formatted list ready to be appended to docs/claude-context.md.
