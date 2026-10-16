#!/usr/bin/env python3
"""Reference token IDs for Ranvier's llama2 chat template (BACKLOG section 29).

The native KV-event stream only answers for a request when Ranvier hashes the
same token IDs the backend stores. For a Llama 2 chat / CodeLlama Instruct model
that means three things line up: Ranvier's `llama2` rendering
(src/chat_template.hpp), the tokenizer Ranvier loads (assets.tokenizer_path ->
tokenizers-cpp), and the backend's own chat template + tokenizer.

This script captures the backend's side: for a fixed set of conversations it
records the token IDs the backend produces, and writes them to a fixture that
tests/unit/llama2_tokenizer_parity_test.cpp replays through Ranvier's real path
(RequestRewriter::extract_text_with_boundary_info + tokenizers-cpp Encode).

Two sources for the reference IDs:

  --vllm URL     POST URL/tokenize with the chat messages (the backend itself;
                 preferred, since it is exactly what vLLM caches)
  --model NAME   transformers AutoTokenizer(NAME).apply_chat_template(...,
                 tokenize=True); needs Hugging Face access to the model

Usage:
    # against a running vLLM backend
    python3 llama2_tokenizer_parity.py --vllm http://localhost:8000 \\
        --model meta-llama/CodeLlama-13b-Instruct-hf --emit-fixture llama2_reference_tokens.json

    # offline, with transformers
    pip install transformers jinja2
    python3 llama2_tokenizer_parity.py --model meta-llama/CodeLlama-13b-Instruct-hf \\
        --source transformers --emit-fixture llama2_reference_tokens.json

Then run the gated C++ test (see README.md).
"""

import argparse
import json
import sys
import urllib.request

# Conversations Ranvier routes on: leading system, system-less, multi-turn, and
# the edge cases where the template's trim and SentencePiece's word-start marker
# could split a token differently at the end of the system block.
CASES = [
    {
        "name": "system_then_user",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "hi"},
        ],
    },
    {
        "name": "user_only",
        "messages": [{"role": "user", "content": "Write a Python function that reverses a list."}],
    },
    {
        "name": "multi_turn",
        "messages": [
            {"role": "system", "content": "You are a senior code reviewer."},
            {"role": "user", "content": "Review this: def f(x): return x*2"},
            {"role": "assistant", "content": "It doubles x. Add a docstring and type hints."},
            {"role": "user", "content": "Show me the revised version."},
        ],
    },
    {
        "name": "user_starts_with_punctuation",
        "messages": [
            {"role": "system", "content": "Answer briefly."},
            {"role": "user", "content": "(a) what is 2+2?"},
        ],
    },
    {
        "name": "whitespace_padding",
        "messages": [
            {"role": "system", "content": "  Indented system prompt.  "},
            {"role": "user", "content": "  padded question \n"},
        ],
    },
    {
        "name": "multiline_system_with_code",
        "messages": [
            {
                "role": "system",
                "content": "You maintain this codebase.\n\n```python\ndef add(a, b):\n    return a + b\n```\n"
                "Follow PEP 8. Never remove tests.",
            },
            {"role": "user", "content": "Add a subtract function."},
        ],
    },
    {
        "name": "unicode",
        "messages": [
            {"role": "system", "content": "Réponds en français, avec précision."},
            {"role": "user", "content": "Qu'est-ce qu'un « tokenizer » ? 日本語も"},
        ],
    },
]


def ids_from_vllm(url, model, messages):
    body = {"messages": messages, "add_generation_prompt": True}
    if model:
        body["model"] = model
    req = urllib.request.Request(
        url.rstrip("/") + "/tokenize",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return [int(t) for t in json.load(resp)["tokens"]]


def ids_from_transformers(tokenizer, messages):
    ids = tokenizer.apply_chat_template(messages, tokenize=True, add_generation_prompt=True)
    return [int(t) for t in ids]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vllm", default=None, help="vLLM base URL (uses POST /tokenize)")
    ap.add_argument("--model", default="meta-llama/CodeLlama-13b-Instruct-hf",
                    help="model name (sent to vLLM; loaded by transformers)")
    ap.add_argument("--source", choices=("vllm", "transformers"), default=None,
                    help="reference source (default: vllm when --vllm is given)")
    ap.add_argument("--emit-fixture", default=None, help="write the fixture JSON here")
    args = ap.parse_args()

    source = args.source or ("vllm" if args.vllm else "transformers")
    if source == "vllm" and not args.vllm:
        ap.error("--source vllm needs --vllm URL")

    tokenizer = None
    if source == "transformers":
        from transformers import AutoTokenizer  # noqa: PLC0415 (optional dependency)
        tokenizer = AutoTokenizer.from_pretrained(args.model)

    cases = []
    for case in CASES:
        if source == "vllm":
            ids = ids_from_vllm(args.vllm, args.model, case["messages"])
        else:
            ids = ids_from_transformers(tokenizer, case["messages"])
        cases.append({"name": case["name"], "messages": case["messages"], "token_ids": ids})
        print(f"{case['name']}: {len(ids)} tokens, first {ids[:10]}")

    fixture = {"model": args.model, "source": source, "cases": cases}
    if args.emit_fixture:
        with open(args.emit_fixture, "w") as f:
            json.dump(fixture, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"wrote {len(cases)} cases to {args.emit_fixture}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
