#!/usr/bin/env python3
"""Kimi tokenizer parity check for Ranvier's prefix-affinity routing.

Ranvier's prefix routing only works if the token IDs it computes for a prompt
match the token IDs the serving backend (vLLM / SGLang) computes for the same
prompt. Ranvier renders the Kimi chat template (see src/chat_template.hpp) and
tokenizes the result with tokenizers-cpp -- i.e. HuggingFace's Rust `tokenizers`
library loading Kimi's fast `tokenizer.json`.

Kimi is natively a *tiktoken* tokenizer; the fast `tokenizer.json` is a secondary
export. If the fast export and the authoritative tokenizer disagree on even one
token, prefix affinity silently degrades (no crash, just a lower cache-hit rate).
This script surfaces that disagreement before it reaches production.

What it compares, over a set of Ranvier-rendered Kimi prompts:

  A. authoritative  -- AutoTokenizer.from_pretrained(MODEL, trust_remote_code=True)
                       (Kimi's own tokenizer; may be the slow/tiktoken path)
  B. ranvier-path   -- tokenizers.Tokenizer.from_file(tokenizer.json)
                       (the fast export tokenizers-cpp actually loads)

It also checks that Ranvier's rendered *string* matches the backend's
apply_chat_template output, so a divergence is attributable to either the
template or the tokenizer, not both at once.

Scope: this exercises Ranvier's supported surface -- string-content system/user/
assistant turns, including the default-system injection. Tool-result framing and
multimodal (image) content are intentionally out of scope; Ranvier does not
reproduce them (see src/request_rewriter.hpp and the README).

Usage:
    pip install transformers tokenizers huggingface_hub jinja2
    python3 kimi_tokenizer_parity.py                       # defaults to K2-Instruct
    python3 kimi_tokenizer_parity.py --model moonshotai/Kimi-K2-Instruct
    python3 kimi_tokenizer_parity.py --tokenizer-json /path/to/tokenizer.json
    python3 kimi_tokenizer_parity.py --emit-fixture kimi_reference_tokens.json

Exit status is nonzero if any prompt fails parity, so it can gate CI.
"""

import argparse
import json
import sys

# --- Ranvier's Kimi rendering, mirrored from src/chat_template.hpp ------------
# Keep in lockstep with ChatTemplate(kimi) + the RequestRewriter default-system
# injection. If the C++ changes, change this too (and vice versa).

KIMI_DEFAULT_SYSTEM = "You are Kimi, an AI assistant created by Moonshot AI."


def _role_token(role):
    if role == "user":
        return "<|im_user|>"
    if role == "assistant":
        return "<|im_assistant|>"
    # system, tool, and anything else open with the system token.
    return "<|im_system|>"


def render_kimi_ranvier(messages, add_generation_prompt=True):
    """Reproduce exactly what Ranvier feeds its tokenizer for a Kimi request."""
    parts = []
    # Default-system injection fires on messages[0], before content filtering,
    # matching the reference jinja's `loop.first and messages[0].role != system`.
    if messages and messages[0].get("role") != "system":
        parts.append(
            f"<|im_system|>system<|im_middle|>{KIMI_DEFAULT_SYSTEM}<|im_end|>"
        )
    for m in messages:
        # Ranvier skips messages whose content is not a plain string.
        content = m.get("content")
        if not isinstance(content, str):
            continue
        role = m.get("role", "user")
        parts.append(f"{_role_token(role)}{role}<|im_middle|>{content}<|im_end|>")
    if add_generation_prompt:
        parts.append("<|im_assistant|>assistant<|im_middle|>")
    return "".join(parts)


# --- Test conversations ------------------------------------------------------
# Deliberately spans: leading system message, no system message (injection),
# multi-turn, and unicode/whitespace edge cases that stress BPE merges.

CONVERSATIONS = [
    [
        {"role": "system", "content": "You are helpful."},
        {"role": "user", "content": "What is 2+2?"},
    ],
    [
        {"role": "user", "content": "Hi"},  # no system -> injection path
    ],
    [
        {"role": "system", "content": "Be concise."},
        {"role": "user", "content": "Explain prefix caching."},
        {"role": "assistant", "content": "It reuses KV blocks for shared prefixes."},
        {"role": "user", "content": "Why does it help TTFT?"},
    ],
    [
        {"role": "user", "content": "Café — naïve façade, 日本語, emoji 🚀\n\ttrailing"},
    ],
    [
        {"role": "system", "content": "You are Kimi."},
        {"role": "user", "content": "   leading spaces and\nnewlines   "},
    ],
]


def _first_divergence(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    if len(a) != len(b):
        return min(len(a), len(b))
    return -1


def _window(ids, idx, radius=4):
    lo = max(0, idx - radius)
    hi = min(len(ids), idx + radius + 1)
    return lo, ids[lo:hi]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="moonshotai/Kimi-K2-Instruct",
                    help="HF model id for the authoritative tokenizer")
    ap.add_argument("--tokenizer-json", default=None,
                    help="Path to the fast tokenizer.json Ranvier loads "
                         "(default: download from --model)")
    ap.add_argument("--emit-fixture", default=None,
                    help="Write reference token IDs to this JSON path for a "
                         "gated C++ test to assert against")
    args = ap.parse_args()

    try:
        from transformers import AutoTokenizer
        import tokenizers
    except ImportError as e:
        print(f"ERROR: missing dependency ({e}).", file=sys.stderr)
        print("Install with: pip install transformers tokenizers huggingface_hub jinja2",
              file=sys.stderr)
        return 2

    print(f"Loading authoritative tokenizer: {args.model}")
    auth = AutoTokenizer.from_pretrained(args.model, trust_remote_code=True)

    # Obtain the fast tokenizer Ranvier's tokenizers-cpp would load. Kimi ships
    # only a tiktoken tokenizer (no tokenizer.json), so this tries, in order:
    # an explicit --tokenizer-json, a tokenizer.json in the model repo, then a
    # transformers-built fast conversion (what an operator must deploy).
    fast = None
    fast_source = None
    if args.tokenizer_json:
        fast = tokenizers.Tokenizer.from_file(args.tokenizer_json)
        fast_source = f"file: {args.tokenizer_json}"
    else:
        try:
            from huggingface_hub import hf_hub_download
            p = hf_hub_download(args.model, "tokenizer.json")
            fast = tokenizers.Tokenizer.from_file(p)
            fast_source = f"hub: {args.model}/tokenizer.json"
        except Exception:  # noqa: BLE001 - absence is expected for Kimi
            print(f"note: {args.model} ships no tokenizer.json; attempting to "
                  "build a fast tokenizer from the authoritative one.")
        if fast is None:
            try:
                fast_tok = AutoTokenizer.from_pretrained(
                    args.model, trust_remote_code=True, use_fast=True)
                if getattr(fast_tok, "is_fast", False):
                    fast = fast_tok.backend_tokenizer
                    fast_source = "converted: AutoTokenizer(use_fast=True)"
            except Exception as e:  # noqa: BLE001
                print(f"note: could not build a fast tokenizer ({e}).")
    if fast is None:
        print("ERROR: no fast tokenizer.json available for the Ranvier path.\n"
              "Kimi ships only a tiktoken tokenizer; tokenizers-cpp needs a fast\n"
              "tokenizer.json. Supply the one you intend to deploy with via\n"
              "--tokenizer-json, or convert the tiktoken tokenizer to HF fast\n"
              "format first (see README).", file=sys.stderr)
        return 2
    print(f"Ranvier-path fast tokenizer -> {fast_source}")

    failures = 0
    fixture = []

    for n, msgs in enumerate(CONVERSATIONS):
        rendered = render_kimi_ranvier(msgs, add_generation_prompt=True)

        # 1) Template-string parity: does Ranvier's render match the backend's?
        ref_str = auth.apply_chat_template(
            msgs, tokenize=False, add_generation_prompt=True)
        str_ok = rendered == ref_str

        # 2) Authoritative token IDs (end-to-end via the reference tokenizer).
        auth_ids = auth.apply_chat_template(
            msgs, tokenize=True, add_generation_prompt=True)

        # 3) Ranvier-path IDs. tokenizers-cpp Encode(text) feeds the already-
        #    rendered string; try both add_special_tokens settings so the report
        #    is unambiguous about which one the C++ Encode must match.
        fast_ids_plain = fast.encode(rendered, add_special_tokens=False).ids
        fast_ids_special = fast.encode(rendered, add_special_tokens=True).ids

        match_plain = fast_ids_plain == auth_ids
        match_special = fast_ids_special == auth_ids
        ids_ok = match_plain or match_special

        status = "PASS" if (str_ok and ids_ok) else "FAIL"
        if status == "FAIL":
            failures += 1
        print(f"[{status}] conversation #{n}  "
              f"(str={'ok' if str_ok else 'MISMATCH'}, "
              f"ids: plain={'ok' if match_plain else 'x'} "
              f"special={'ok' if match_special else 'x'})")

        if not str_ok:
            print("   rendered != backend apply_chat_template:")
            print(f"     ranvier : {rendered!r}")
            print(f"     backend : {ref_str!r}")

        if not ids_ok:
            idx = _first_divergence(auth_ids, fast_ids_plain)
            lo, aw = _window(auth_ids, idx)
            _, fw = _window(fast_ids_plain, idx)
            print(f"   token IDs diverge near index {idx} (from {lo}):")
            print(f"     authoritative: {aw}")
            print(f"     ranvier/plain: {fw}")
            print(f"     len auth={len(auth_ids)} plain={len(fast_ids_plain)} "
                  f"special={len(fast_ids_special)}")

        fixture.append({
            "messages": msgs,
            "rendered": rendered,
            "token_ids": auth_ids,
        })

    if args.emit_fixture:
        with open(args.emit_fixture, "w", encoding="utf-8") as fh:
            json.dump({"model": args.model, "cases": fixture}, fh,
                      ensure_ascii=False, indent=2)
        print(f"\nWrote fixture: {args.emit_fixture} ({len(fixture)} cases)")

    total = len(CONVERSATIONS)
    print(f"\n{total - failures}/{total} conversations passed parity.")
    if failures:
        print("Parity FAILED. If only add_special_tokens=special matched, "
              "Ranvier's tokenizers-cpp Encode must add special tokens (or the "
              "reverse). If the string mismatched, reconcile the chat template.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
