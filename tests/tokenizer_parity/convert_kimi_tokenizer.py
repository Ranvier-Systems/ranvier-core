#!/usr/bin/env python3
"""Convert Kimi's tiktoken tokenizer to an HF fast tokenizer.json for Ranvier.

Kimi ships only a tiktoken tokenizer (no tokenizer.json), and no public mirror
provides a fast export. Ranvier's tokenizers-cpp needs a fast tokenizer.json, so
a conversion is a hard prerequisite for running Kimi on Ranvier.

This extracts the `tiktoken.Encoding` that Kimi's own tokenizer builds -- reusing
its exact merge ranks, split regex, and special tokens -- and writes a fast
tokenizer.json via transformers' convert_tiktoken_to_fast.

A conversion is only useful if it reproduces the authoritative token IDs, so
ALWAYS validate the output before deploying:

    python3 kimi_tokenizer_parity.py --tokenizer-json <out>/tokenizer.json

If parity passes, that tokenizer.json is the contract: Ranvier's `tokenizer_path`
AND the serving backend must both load it, or the two sides won't share cache.

Usage:
    pip install transformers tokenizers tiktoken blobfile
    python3 convert_kimi_tokenizer.py --model moonshotai/Kimi-K2-Instruct --out kimi_fast
"""

import argparse
import sys


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="moonshotai/Kimi-K2-Instruct",
                    help="HF model id whose tiktoken tokenizer to convert")
    ap.add_argument("--out", default="kimi_fast",
                    help="Output directory for the fast tokenizer.json")
    args = ap.parse_args()

    try:
        import tiktoken
        from transformers import AutoTokenizer
    except ImportError as e:
        print(f"missing dependency ({e}). "
              "pip install transformers tokenizers tiktoken", file=sys.stderr)
        return 2

    print(f"Loading slow (tiktoken) tokenizer: {args.model}")
    slow = AutoTokenizer.from_pretrained(args.model, trust_remote_code=True)

    # Locate the tiktoken.Encoding the slow tokenizer built. Reusing it (rather
    # than reconstructing ranks/regex/specials by hand) is what keeps the
    # conversion faithful to Kimi's exact tokenization.
    enc = None
    found_at = None
    for name in dir(slow):
        try:
            value = getattr(slow, name)
        except Exception:  # noqa: BLE001 - some attrs raise on access
            continue
        if isinstance(value, tiktoken.Encoding):
            enc, found_at = value, name
            break
    if enc is None:
        print("ERROR: no tiktoken.Encoding found on the tokenizer object.\n"
              "Inspect dir(slow) and tokenization_kimi.py to find where the\n"
              "Encoding lives, then adjust this script.", file=sys.stderr)
        return 3
    print(f"Found tiktoken.Encoding at attribute: {found_at} "
          f"(n_vocab={enc.n_vocab}, {len(enc._special_tokens)} special tokens)")

    try:
        from transformers.integrations.tiktoken import convert_tiktoken_to_fast
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: convert_tiktoken_to_fast unavailable ({e}); "
              "upgrade transformers.", file=sys.stderr)
        return 3

    print(f"Writing fast tokenizer -> {args.out}/tokenizer.json")
    convert_tiktoken_to_fast(enc, args.out)

    print("\nDone. VALIDATE before trusting it:")
    print(f"  python3 kimi_tokenizer_parity.py --tokenizer-json {args.out}/tokenizer.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
