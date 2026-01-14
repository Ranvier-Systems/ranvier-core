1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

STAGED EXECUTION:
Pass 1: Logic & Correctness (Handle edge cases/robustness).
Pass 2: Refactor for clarity and modularity.
Pass 3: Optimize for performance within our Async constraints.

OUTPUT: Provide the final code for the modified files. Explain any architectural trade-offs.

1. Static Analysis Only: Do not attempt to run cmake or build in your environment. Our dependencies (Seastar) are too heavy for the sandbox.
2. API Verification: Verify syntax against the Seastar documentation logic.
3. Manual Verification: I will handle the build in my Docker container and provide you with the logs if it fails.

I am STARTING AN IMPLEMENTATION TASK: 
<INSERT_TASK_FROM_TODO_LIST>

