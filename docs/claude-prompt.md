1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. **Static Analysis Only:** Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. **API Verification:** Verify syntax against Seastar documentation logic.
3. **Manual Verification:** I will build in my Docker container and provide logs if it fails.

## Quick Reference (before every task)

Before writing any code, verify:
- [ ] Have I read the relevant file(s) I'm about to modify?
- [ ] Does this touch cross-shard communication? → Use `seastar::smp::submit_to`
- [ ] Is there a MAX_SIZE for any new container?
- [ ] Any new timer/callback? → Needs gate guard
- [ ] Any new metrics lambda capturing `this`? → Deregister in `stop()`
- [ ] Any C API string returns? → Null-guard required

---

I am starting a task:

