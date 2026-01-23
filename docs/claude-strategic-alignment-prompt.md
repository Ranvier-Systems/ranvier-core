1. Ref docs/claude-context.md for core project essence and "No Locks/Async Only" rules.
2. DO NOT read /assets or /docs (except for TODO.md and claude-context.md).
3. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

I am requesting a STRATEGIC PROJECT ASSESSMENT.

Act as an External CTO and Lead Architect. I need a "brutally honest" evaluation of where Ranvier Core stands today versus our stated goal of "Layer 7+ load balancer with Prefix Caching."

TASK: Analyze the current 33-file structure and docs/TODO.md to answer:

1. THE "GOAL ALIGNMENT" CHECK:
- Based on the code implemented, are we actually building a Prefix Caching balancer, or have we drifted into a general-purpose proxy?
- Are the core "Prefix Logic" constraints (Constraint #3) actually reflected in the code, or are they just aspirations in the context doc?

2. THE "COMPLEXITY VS. VALUE" AUDIT:
- Where is the highest concentration of complexity (e.g., "The Sprawling Module")? Is that complexity buying us performance, or is it technical debt?
- Are we over-engineering the shard-broadcast logic relative to the current feature set?

3. THE "HIDDEN FRAGILITY" ASSESSMENT:
- Identify the "Load-Bearing" files—if these fail, the whole system collapses. Are these files sufficiently guarded by our list of Hard Rules?
- Based on the TODO list, what is the "Next Big Risk" we are about to take on?

4. THE "STAFF ENGINEER" RECOMMENDATION:
- If you were taking over this project today, what is the one thing you would delete, and the one thing you would refactor immediately to reach production readiness?

OUTPUT: Provide a "State of the Project" scorecard (A-F) for Architecture, Reliability, and Progress-to-Goal, followed by the prioritized analysis.
