#!/usr/bin/env python3
"""
Prompt File Loader for LLM Benchmarks

A standalone module for loading prompts from JSONL files in various formats.
Can be used as a library or as a CLI tool for validation.

Supported Formats:
    - ShareGPT: {"conversations": [{"from": "human", "value": "..."}, ...]}
    - OpenAI:   {"messages": [{"role": "user", "content": "..."}, ...]}
    - Simple:   {"prompt": "user prompt text here"}

Usage as CLI:
    # Validate a prompt file
    python prompt_loader.py validate prompts.jsonl

    # Show statistics about a prompt file
    python prompt_loader.py stats prompts.jsonl

    # Preview prompts (first N)
    python prompt_loader.py preview prompts.jsonl --count 5

    # Convert between formats (output to stdout)
    python prompt_loader.py convert prompts.jsonl --format openai

Usage as Library:
    from prompt_loader import PromptLoader

    loader = PromptLoader()
    loader.load_file("prompts.jsonl")
    prompt = loader.get_prompt()  # Returns (messages, prefix_hash, token_count)

Official Datasets (ShareGPT format):
    - ShareGPT: https://huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered
    - LMSYS-Chat-1M: https://huggingface.co/datasets/lmsys/lmsys-chat-1m
    - WildChat: https://huggingface.co/datasets/allenai/WildChat-1M
    - OpenOrca: https://huggingface.co/datasets/Open-Orca/OpenOrca

Download example:
    # Using huggingface-cli
    pip install huggingface_hub
    huggingface-cli download lmsys/lmsys-chat-1m --include "*.jsonl" --local-dir ./data

    # Or direct download (ShareGPT sample)
    wget https://huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered/resolve/main/ShareGPT_V3_unfiltered_cleaned_split.json
"""

import argparse
import json
import os
import random
import sys
from dataclasses import dataclass, field
from threading import Lock
from typing import Any, Dict, List, Optional, Tuple


@dataclass
class LoadedPrompt:
    """A prompt loaded from file with metadata."""
    messages: List[dict]
    prefix_hash: str
    estimated_tokens: int
    source_format: str  # "sharegpt", "openai", "simple"
    prefix_group: Optional[str] = None
    line_number: int = 0


@dataclass
class LoadStats:
    """Statistics about loaded prompts."""
    total_loaded: int = 0
    total_failed: int = 0
    format_counts: Dict[str, int] = field(default_factory=lambda: {"sharegpt": 0, "openai": 0, "simple": 0})
    prefix_groups: int = 0
    avg_tokens: int = 0
    min_tokens: int = 0
    max_tokens: int = 0
    errors: List[str] = field(default_factory=list)


def parse_sharegpt_format(data: dict) -> Optional[List[dict]]:
    """Parse ShareGPT conversation format.

    Format: {"conversations": [{"from": "human", "value": "..."}, {"from": "gpt", "value": "..."}]}

    Returns OpenAI-style messages list.
    """
    if "conversations" not in data:
        return None

    conversations = data["conversations"]
    if not isinstance(conversations, list) or not conversations:
        return None

    messages = []
    for turn in conversations:
        if not isinstance(turn, dict):
            continue

        from_role = turn.get("from", "")
        value = turn.get("value", "")

        if not value:
            continue

        # Map ShareGPT roles to OpenAI roles
        if from_role in ("human", "user"):
            messages.append({"role": "user", "content": value})
        elif from_role in ("gpt", "assistant", "model"):
            messages.append({"role": "assistant", "content": value})
        elif from_role == "system":
            messages.append({"role": "system", "content": value})

    return messages if messages else None


def parse_openai_format(data: dict) -> Optional[List[dict]]:
    """Parse OpenAI messages format.

    Format: {"messages": [{"role": "user", "content": "..."}, {"role": "assistant", "content": "..."}]}

    Returns OpenAI-style messages list.
    """
    if "messages" not in data:
        return None

    messages = data["messages"]
    if not isinstance(messages, list) or not messages:
        return None

    # Validate and clean messages
    cleaned = []
    for msg in messages:
        if not isinstance(msg, dict):
            continue
        if "role" not in msg or "content" not in msg:
            continue
        if msg["role"] not in ("user", "assistant", "system"):
            continue
        if not isinstance(msg["content"], str):
            continue
        cleaned.append({"role": msg["role"], "content": msg["content"]})

    return cleaned if cleaned else None


def parse_simple_format(data: dict) -> Optional[List[dict]]:
    """Parse simple prompt format.

    Format: {"prompt": "user prompt text here"}

    Returns OpenAI-style messages list with single user message.
    """
    prompt = data.get("prompt")
    if not prompt or not isinstance(prompt, str):
        return None

    return [{"role": "user", "content": prompt}]


def parse_prompt_line(line: str) -> Tuple[Optional[List[dict]], str, Optional[str]]:
    """Parse a single line from the prompt file.

    Tries each format in order: ShareGPT, OpenAI messages, simple.

    Returns:
        Tuple of (messages, format_name, error_message)
    """
    try:
        data = json.loads(line)
        if not isinstance(data, dict):
            return None, "", "Not a JSON object"
    except json.JSONDecodeError as e:
        return None, "", f"JSON parse error: {e}"

    # Try ShareGPT format first (most common for LLM benchmarks)
    messages = parse_sharegpt_format(data)
    if messages:
        return messages, "sharegpt", None

    # Try OpenAI messages format
    messages = parse_openai_format(data)
    if messages:
        return messages, "openai", None

    # Try simple format
    messages = parse_simple_format(data)
    if messages:
        return messages, "simple", None

    return None, "", "No recognized format (expected 'conversations', 'messages', or 'prompt' key)"


def estimate_tokens(text: str) -> int:
    """Estimate token count from text length (~4 chars per token)."""
    return len(text) // 4


def compute_prefix_hash(messages: List[dict]) -> str:
    """Compute a prefix hash for cache tracking.

    Only hashes the shared prefix portion (system prompt + any assistant context),
    excluding the final user message which is typically unique per request.
    This allows conversations with the same system prompt to share cache entries.
    """
    if not messages:
        return str(hash(""))

    # For single message, use the first portion of it
    if len(messages) == 1:
        content = messages[0].get("content", "")
        return str(hash(content[:800]))

    # For multi-message: hash everything EXCEPT the last message
    # (the last message is typically the unique user query)
    prefix_text = ""
    for msg in messages[:-1]:
        role = msg.get("role", "")
        content = msg.get("content", "")
        prefix_text += f"{role}: {content}\n"

    # If prefix is empty (e.g., only user messages), use first message
    if not prefix_text.strip():
        content = messages[0].get("content", "")
        return str(hash(content[:800]))

    return str(hash(prefix_text[:1600]))


def extract_prefix_group(messages: List[dict]) -> str:
    """Extract a prefix group identifier from messages."""
    # Check for system message first
    for msg in messages:
        if msg["role"] == "system":
            return msg["content"][:400]

    # Fall back to first user message
    for msg in messages:
        if msg["role"] == "user":
            return msg["content"][:400]

    return ""


class PromptLoader:
    """Load and manage prompts from JSONL files."""

    def __init__(self, shared_prefix_ratio: float = 0.7, sampling: str = "random"):
        """Initialize the prompt loader.

        Args:
            shared_prefix_ratio: Ratio of requests that should use shared prefix grouping (0.0-1.0)
            sampling: Sampling strategy - "random", "sequential", or "weighted"
        """
        self.shared_prefix_ratio = shared_prefix_ratio
        self.sampling = sampling.lower()

        self._prompts: List[LoadedPrompt] = []
        self._prompts_by_prefix: Dict[str, List[LoadedPrompt]] = {}
        self._loaded = False
        self._prompt_index = 0
        self._lock = Lock()
        self._stats = LoadStats()

    def load_file(self, file_path: str) -> LoadStats:
        """Load prompts from a JSONL file.

        Args:
            file_path: Path to the JSONL file

        Returns:
            LoadStats with loading statistics
        """
        if self._loaded:
            return self._stats

        self._stats = LoadStats()

        if not file_path or not os.path.exists(file_path):
            self._stats.errors.append(f"File not found: {file_path}")
            self._loaded = True
            return self._stats

        prefix_groups: Dict[str, List[LoadedPrompt]] = {}
        token_counts: List[int] = []

        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                for line_num, line in enumerate(f, 1):
                    line = line.strip()
                    if not line:
                        continue

                    messages, source_format, error = parse_prompt_line(line)
                    if messages is None:
                        self._stats.total_failed += 1
                        if len(self._stats.errors) < 10:
                            self._stats.errors.append(f"Line {line_num}: {error}")
                        continue

                    self._stats.format_counts[source_format] += 1

                    # Calculate metadata
                    full_text = " ".join(m["content"] for m in messages)
                    est_tokens = estimate_tokens(full_text)
                    token_counts.append(est_tokens)

                    prefix_hash = compute_prefix_hash(messages)
                    prefix_group = extract_prefix_group(messages)

                    prompt = LoadedPrompt(
                        messages=messages,
                        prefix_hash=prefix_hash,
                        estimated_tokens=est_tokens,
                        source_format=source_format,
                        prefix_group=prefix_group,
                        line_number=line_num
                    )

                    self._prompts.append(prompt)

                    if prefix_group not in prefix_groups:
                        prefix_groups[prefix_group] = []
                    prefix_groups[prefix_group].append(prompt)

                    self._stats.total_loaded += 1

        except Exception as e:
            self._stats.errors.append(f"Error reading file: {e}")

        self._prompts_by_prefix = prefix_groups
        self._loaded = True

        # Calculate stats
        if token_counts:
            self._stats.avg_tokens = sum(token_counts) // len(token_counts)
            self._stats.min_tokens = min(token_counts)
            self._stats.max_tokens = max(token_counts)
        self._stats.prefix_groups = len(prefix_groups)

        return self._stats

    def get_prompt(self) -> Optional[Tuple[List[dict], str, int]]:
        """Get a prompt using the configured sampling strategy.

        Returns:
            Tuple of (messages, prefix_hash, estimated_tokens) or None if no prompts loaded.
        """
        if not self._prompts:
            return None

        # Decide whether to use shared prefix grouping
        use_shared_prefix = random.random() < self.shared_prefix_ratio

        if use_shared_prefix and self._prompts_by_prefix:
            prefix_groups = list(self._prompts_by_prefix.keys())
            if prefix_groups:
                group_key = random.choice(prefix_groups)
                prompts_in_group = self._prompts_by_prefix[group_key]
                if prompts_in_group:
                    prompt = random.choice(prompts_in_group)
                    return prompt.messages, prompt.prefix_hash, prompt.estimated_tokens

        # Regular sampling based on strategy
        if self.sampling == "sequential":
            with self._lock:
                prompt = self._prompts[self._prompt_index % len(self._prompts)]
                self._prompt_index += 1
            return prompt.messages, prompt.prefix_hash, prompt.estimated_tokens

        elif self.sampling == "weighted":
            total_tokens = sum(p.estimated_tokens for p in self._prompts)
            if total_tokens == 0:
                prompt = random.choice(self._prompts)
            else:
                r = random.uniform(0, total_tokens)
                cumulative = 0
                prompt = self._prompts[-1]
                for p in self._prompts:
                    cumulative += p.estimated_tokens
                    if r <= cumulative:
                        prompt = p
                        break
            return prompt.messages, prompt.prefix_hash, prompt.estimated_tokens

        else:  # random (default)
            prompt = random.choice(self._prompts)
            return prompt.messages, prompt.prefix_hash, prompt.estimated_tokens

    def get_stats(self) -> LoadStats:
        """Get loading statistics."""
        return self._stats

    @property
    def prompts(self) -> List[LoadedPrompt]:
        """Get all loaded prompts."""
        return self._prompts

    @property
    def is_loaded(self) -> bool:
        """Check if prompts have been loaded."""
        return self._loaded

    def __len__(self) -> int:
        return len(self._prompts)


# =============================================================================
# CLI Interface
# =============================================================================

def cmd_validate(args: argparse.Namespace) -> int:
    """Validate a prompt file."""
    loader = PromptLoader()
    stats = loader.load_file(args.file)

    print(f"Validating: {args.file}")
    print()

    if stats.total_loaded == 0 and stats.total_failed == 0:
        print("ERROR: File not found or empty")
        return 1

    print(f"Results:")
    print(f"  Valid prompts:   {stats.total_loaded}")
    print(f"  Invalid lines:   {stats.total_failed}")

    if stats.total_loaded > 0:
        print(f"\nFormat breakdown:")
        for fmt, count in stats.format_counts.items():
            if count > 0:
                print(f"  {fmt:10s}: {count}")

    if stats.errors:
        print(f"\nErrors (showing first {min(len(stats.errors), 10)}):")
        for error in stats.errors[:10]:
            print(f"  {error}")

    if stats.total_failed > 0:
        print(f"\nValidation FAILED: {stats.total_failed} invalid lines")
        return 1

    print(f"\nValidation PASSED: All {stats.total_loaded} prompts are valid")
    return 0


def cmd_stats(args: argparse.Namespace) -> int:
    """Show statistics about a prompt file."""
    loader = PromptLoader()
    stats = loader.load_file(args.file)

    if stats.total_loaded == 0:
        print(f"ERROR: No valid prompts loaded from {args.file}")
        if stats.errors:
            for error in stats.errors[:5]:
                print(f"  {error}")
        return 1

    print(f"Statistics for: {args.file}")
    print()
    print(f"Prompts:")
    print(f"  Total loaded:    {stats.total_loaded}")
    print(f"  Failed to parse: {stats.total_failed}")
    print()
    print(f"Formats:")
    for fmt, count in stats.format_counts.items():
        if count > 0:
            pct = 100 * count / stats.total_loaded
            print(f"  {fmt:10s}: {count:5d} ({pct:.1f}%)")
    print()
    print(f"Tokens (estimated):")
    print(f"  Average: {stats.avg_tokens}")
    print(f"  Min:     {stats.min_tokens}")
    print(f"  Max:     {stats.max_tokens}")
    print()
    print(f"Prefix Groups: {stats.prefix_groups}")
    print(f"  (Prompts with shared system messages or similar prefixes)")

    return 0


def cmd_preview(args: argparse.Namespace) -> int:
    """Preview prompts from a file."""
    loader = PromptLoader()
    stats = loader.load_file(args.file)

    if stats.total_loaded == 0:
        print(f"ERROR: No valid prompts loaded from {args.file}")
        return 1

    count = min(args.count, len(loader))
    print(f"Previewing first {count} prompts from {args.file}:")
    print()

    for i, prompt in enumerate(loader.prompts[:count]):
        print(f"--- Prompt {i + 1} (line {prompt.line_number}, {prompt.source_format}, ~{prompt.estimated_tokens} tokens) ---")
        for msg in prompt.messages:
            role = msg["role"].upper()
            content = msg["content"]
            if len(content) > 200:
                content = content[:200] + "..."
            print(f"[{role}]: {content}")
        print()

    return 0


def cmd_convert(args: argparse.Namespace) -> int:
    """Convert prompts to a different format."""
    loader = PromptLoader()
    stats = loader.load_file(args.file)

    if stats.total_loaded == 0:
        print(f"ERROR: No valid prompts loaded from {args.file}", file=sys.stderr)
        return 1

    output_format = args.format.lower()

    for prompt in loader.prompts:
        if output_format == "openai":
            output = {"messages": prompt.messages}
        elif output_format == "sharegpt":
            conversations = []
            for msg in prompt.messages:
                role_map = {"user": "human", "assistant": "gpt", "system": "system"}
                conversations.append({
                    "from": role_map.get(msg["role"], msg["role"]),
                    "value": msg["content"]
                })
            output = {"conversations": conversations}
        elif output_format == "simple":
            # Only convert single user message prompts
            user_msgs = [m for m in prompt.messages if m["role"] == "user"]
            if len(user_msgs) == 1 and len(prompt.messages) == 1:
                output = {"prompt": user_msgs[0]["content"]}
            else:
                # Fall back to OpenAI format for multi-turn
                output = {"messages": prompt.messages}
        else:
            print(f"ERROR: Unknown format '{output_format}'", file=sys.stderr)
            return 1

        print(json.dumps(output))

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Prompt file loader and validator for LLM benchmarks",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s validate prompts.jsonl
  %(prog)s stats prompts.jsonl
  %(prog)s preview prompts.jsonl --count 3
  %(prog)s convert prompts.jsonl --format openai > converted.jsonl

Official Datasets (ShareGPT format):
  - ShareGPT: huggingface.co/datasets/anon8231489123/ShareGPT_Vicuna_unfiltered
  - LMSYS-Chat-1M: huggingface.co/datasets/lmsys/lmsys-chat-1m
  - WildChat: huggingface.co/datasets/allenai/WildChat-1M
"""
    )

    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # validate command
    p_validate = subparsers.add_parser("validate", help="Validate a prompt file")
    p_validate.add_argument("file", help="Path to JSONL file")

    # stats command
    p_stats = subparsers.add_parser("stats", help="Show statistics about a prompt file")
    p_stats.add_argument("file", help="Path to JSONL file")

    # preview command
    p_preview = subparsers.add_parser("preview", help="Preview prompts from a file")
    p_preview.add_argument("file", help="Path to JSONL file")
    p_preview.add_argument("--count", "-n", type=int, default=5, help="Number of prompts to preview")

    # convert command
    p_convert = subparsers.add_parser("convert", help="Convert prompts to a different format")
    p_convert.add_argument("file", help="Path to JSONL file")
    p_convert.add_argument("--format", "-f", choices=["openai", "sharegpt", "simple"],
                          default="openai", help="Output format")

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        return 1

    commands = {
        "validate": cmd_validate,
        "stats": cmd_stats,
        "preview": cmd_preview,
        "convert": cmd_convert,
    }

    return commands[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
