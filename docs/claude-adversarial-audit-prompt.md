1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

I am requesting an ADVERSARIAL SYSTEM AUDIT of the source files under src/

TASK: Act as a cynical Staff Engineer and Security Auditor. Do not look for "small issues"; look for structural failures. Perform the audit across these 4 lenses:

1. THE "ASYNC INTEGRITY" LENS:
- Scan for any hidden blocking calls, `await` inside loops that should be `Promise.all`, or "async-to-sync" wrappers that could cause deadlocks.

2. THE "EDGE-CASE CRASH" LENS:
- If a network call fails, a database returns null, or an input is malformed, exactly where will this code throw an unhandled exception? Point to the specific lines.

3. THE "ARCHITECTURE DRIFT" LENS:
- Does this code bypass our layers (e.g., UI calling DB directly)? 
- Is there "Logic Leakage" where business rules are being handled in utility files?

4. THE "SCALE & LEAK" LENS:
- Will this logic hold up if the data size grows 100x? 
- Are there any event listeners or observers being added without being cleaned up?

OUTPUT: Give me a "Criticality Score" (1-10) for the current state and a bulleted list of "Structural Fixes" to add to the docs/TODO.md list.
