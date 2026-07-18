# Kimi tokenizer parity

Ranvier's prefix-affinity routing is only correct if the token IDs Ranvier
computes for a prompt are **identical** to the token IDs the serving backend
(vLLM / SGLang) computes for the same prompt. The chat-template work
(`src/chat_template.hpp`) makes Ranvier's *rendered string* match the backend's
`apply_chat_template`; this harness closes the other half — that the *tokenizer*
turns that string into the same IDs.

## Why Kimi specifically needs this

Kimi is natively a **tiktoken** tokenizer. The fast `tokenizer.json` that
Ranvier loads (via `tokenizers-cpp` → HuggingFace's Rust `tokenizers`) is a
secondary export. If the fast export and the authoritative tokenizer disagree on
any token, prefix affinity degrades **silently** — no crash, no failing build,
just a lower cache-hit ratio. Unit tests can't catch this because they don't run
a real Kimi tokenizer; this harness does.

## Kimi ships no fast `tokenizer.json`

`moonshotai/Kimi-K2-Instruct` provides only `tiktoken.model` (plus
`tokenization_kimi.py` and `chat_template.jinja`) — there is **no** HuggingFace
fast `tokenizer.json` in the repo. Ranvier's `tokenizers-cpp` cannot load a
tiktoken model; it needs a fast `tokenizer.json`. So **deploying Kimi on Ranvier
requires converting** the tiktoken tokenizer to HF fast format, and the fidelity
of that conversion is the routing risk this harness measures.

The harness tries, in order: an explicit `--tokenizer-json`, a `tokenizer.json`
in the model repo, then a transformers-built fast conversion
(`AutoTokenizer(use_fast=True)`). For Kimi K2 all three fail — the repo has none,
no public mirror ships one, and the custom tiktoken tokenizer has no auto-built
fast variant. Produce a first-party conversion instead:

```bash
pip install transformers tokenizers tiktoken blobfile
python3 convert_kimi_tokenizer.py --model moonshotai/Kimi-K2-Instruct --out kimi_fast
python3 kimi_tokenizer_parity.py --tokenizer-json kimi_fast/tokenizer.json
```

(The conversion also needs `blobfile` — tiktoken pulls it in to serialize the
BPE ranks.)

`convert_kimi_tokenizer.py` extracts the exact `tiktoken.Encoding` Kimi's own
tokenizer builds and writes a fast `tokenizer.json`. It is only trustworthy once
the harness confirms it reproduces the authoritative IDs. Whatever `tokenizer.json`
passes here is the contract: Ranvier's `tokenizer_path` **and** the serving
backend must both load it, or the two sides won't share cache.

## What it checks

For a set of Ranvier-rendered Kimi prompts (leading-system, no-system/injection,
multi-turn, and unicode/whitespace edge cases), it compares:

- **A. authoritative** — `AutoTokenizer.from_pretrained(MODEL, trust_remote_code=True)`
  (Kimi's own tokenizer, which may be the slow/tiktoken path).
- **B. Ranvier path** — `tokenizers.Tokenizer.from_file(tokenizer.json)`, the fast
  export `tokenizers-cpp` actually loads.

It also asserts Ranvier's rendered string equals the backend's
`apply_chat_template` output, so any failure is attributable to the template *or*
the tokenizer, not both.

It tries both `add_special_tokens=False` and `True` on the Ranvier path and
reports which matched — that tells you exactly what the C++ `Encode(text)` call
must do (see `src/tokenizer_service.cpp` / `src/tokenizer_thread_pool.cpp`).

## Running

```bash
pip install transformers tokenizers huggingface_hub
python3 kimi_tokenizer_parity.py                      # defaults to Kimi-K2-Instruct
# or point at a local fast tokenizer.json:
python3 kimi_tokenizer_parity.py --tokenizer-json /path/to/tokenizer.json
```

Exit status is nonzero if any prompt fails parity, so it can gate CI on a runner
that has the model files. When **K3** ships, rerun with `--model <k3-id>` to
confirm its tokenizer/template haven't shifted.

## Interpreting failures

- **string mismatch** → the chat template diverged (reconcile `src/chat_template.hpp`
  and `tests/unit/chat_template_test.cpp` against the new `chat_template.jinja`).
- **only `add_special_tokens=special` matched** → Ranvier's `Encode` must add
  special tokens (or vice versa); the plain/special columns pinpoint which.
- **IDs diverge mid-sequence with matching string** → the fast `tokenizer.json`
  and the authoritative tiktoken tokenizer genuinely disagree. This is the case
  that blocks routing: Ranvier (fast export) will only align with a backend that
  also uses the fast export. Confirm which tokenizer your vLLM/SGLang build loads.

## Emitting a fixture for a gated C++ test

```bash
python3 kimi_tokenizer_parity.py --emit-fixture kimi_reference_tokens.json
```

This writes each case's authoritative token IDs. A follow-up Google Test can load
a Kimi `tokenizer.json` (path via env var, skipped when absent) and assert
`tokenizers-cpp` reproduces these IDs — moving the check into the real FFI path
Ranvier uses, on every build. Not built yet; see the project backlog.

## Out of scope (by design)

Tool-result turns (the `## Return of {id}` wrapper) and multimodal/image content
are **not** reproduced by Ranvier's formatter — image parts are dropped at
`src/request_rewriter.hpp` (non-string `content` is skipped). Those cases are
excluded here so parity reflects Ranvier's actual routing surface. If K3
multimodal traffic becomes a target, that gap needs its own design.
