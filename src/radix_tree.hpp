#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <variant>
#include <array>
#include <memory>
#include <span>
#include <iostream>

/*
The Design Philosophy
It is designed with C++20 features (Concepts, Spans) and focuses on the Adaptive Node layout:
- Path Compression: Every node stores a prefix vector. We skip checking individual nodes if the prefix matches.
- Node Types: We define Node4, Node16, etc., but for this skeleton, I have implemented the Base and Node4 (the most common node) to get started.
- Zero-Copy Lookups: We use std::span<int32_t> for the lookup key so we don't copy vectors around.

Why this structure?
- std::span (insert function): When we slice the input tokens recursively, we don't want to copy the vector every time. span is just a pointer and a length. It makes the recursion nearly free.
- std::vector<TokenId> (Node struct): This implements the Path Compression. If the system prompt is 1,000 tokens, this vector holds them. The insert_recursive logic checks this first.
- split_node: It handles the "Conflict" scenario where a lazy path needs to branch.
*/

namespace ranvier {

// 1. Types
// Token IDs are 32-bit integers from the Tokenizer
using TokenId = int32_t;
// Backend ID is the GPU Pool ID (0, 1, 2...)
using BackendId = int16_t;

// 2. Node Types for Adaptive Radix Tree
// We use a "Tagged Union" style (via variant or inheritance) to save memory.
enum class NodeType : uint8_t {
    Node4,
    Node16,
    Node48,   // (Advanced) Uses an index array
    Node256   // (Advanced) Direct lookup array
};

// 3. The Base Node (Header)
// Every node starts with this. It fits in a single cache line.
struct Node {
    NodeType type;

    // OPTIMISTIC PATH COMPRESSION
    // Instead of a chain of nodes A->B->C, we store [A,B,C] here.
    // If the input matches this prefix, we jump straight to children.
    std::vector<TokenId> prefix;

    // Leaf Data: If this node terminates a valid route, this has a value.
    std::optional<BackendId> leaf_value;

    Node(NodeType t) : type(t) {}
    virtual ~Node() = default;
};

// 4. Node4: Smallest Node (Holds up to 4 children)
// Used when branching factor is low (e.g. "User" -> "A", "B")
// Layout: [Header] + [4 Keys] + [4 Pointers]
// This is "Structure of Arrays" (SoA) friendly for SIMD (future optimization).
struct Node4 : public Node {
    std::vector<TokenId> keys;
    std::vector<std::shared_ptr<Node>> children;

    Node4() : Node(NodeType::Node4) {
        keys.reserve(4);
        children.reserve(4);
    }
};

// 5. Node256: Largest Node (Direct Access)
// Used when a node is very hot (e.g. the root of the tree)
struct Node256 : public Node {
    // A hash map or direct array could go here.
    // For v0.1, we can just use a larger vector or map.
    // In production ART, this is usually `Node* children[256]` mapped by byte.
    // Since tokens are 32-bit, we might use a std::map for the "Root" mostly.
    // (Implementation deferred for v0.1)
};

// 6. The Tree Manager
class RadixTree {
public:
    RadixTree() {
        root = std::make_shared<Node4>(); // Start small
    }

    // INSERT
    // O(k) where k is the number of tokens
    void insert(std::span<const TokenId> tokens, BackendId backend) {
        insert_recursive(root, tokens, backend);
    }

    // LOOKUP
    // Returns the BackendId if found, or nullopt
    std::optional<BackendId> lookup(std::span<const TokenId> tokens) {
        return lookup_recursive(root, tokens);
    }

private:
    std::shared_ptr<Node> root;

    // Recursive Insert Logic (The "Hard" Part)
    void insert_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens, BackendId backend) {
        // 1. Calculate matching prefix length between Node's stored prefix and Input
        size_t match_len = 0;
        while(match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) break;
            match_len++;
        }

        // CASE A: Node has a prefix "ABC", Input is "ABD..."
        // We need to SPLIT this node.
        if (match_len < node->prefix.size()) {
            split_node(node, match_len);
        }

        // Update the span to skip the matched part
        auto remaining_tokens = tokens.subspan(match_len);

        // CASE B: Exact Match / End of Input
        if (remaining_tokens.empty()) {
            node->leaf_value = backend;
            return;
        }

        // CASE C: Need to traverse to a child
        TokenId next_token = remaining_tokens[0];
        auto child = find_child(node, next_token);

        if (child) {
            // Recurse down
            insert_recursive(child, remaining_tokens.subspan(1), backend);
        } else {
            // Create new child (Lazy Expansion)
            auto new_child = std::make_shared<Node4>();

            // Store the REST of the tokens in the new child's prefix (Lazy)
            if (remaining_tokens.size() > 1) {
                auto suffix = remaining_tokens.subspan(1);
                new_child->prefix.assign(suffix.begin(), suffix.end());
            }

            new_child->leaf_value = backend;
            add_child(node, next_token, new_child);
        }
    }

    // Helper: Find child by TokenID in a Node4
    std::shared_ptr<Node> find_child(std::shared_ptr<Node> node, TokenId key) {
        // We assume Node4 for now. In full impl, switch(node->type)
        if (node->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(node.get());
            for (size_t i = 0; i < n4->keys.size(); i++) {
                if (n4->keys[i] == key) return n4->children[i];
            }
        }
        return nullptr;
    }

    // Helper: Add child to Node4 (Naive implementation without resizing)
    void add_child(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
         if (parent->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(parent.get());
            // TODO: In real ART, check size < 4. If full, grow to Node16.
            n4->keys.push_back(key);
            n4->children.push_back(child);
        }
    }

    // Helper: Split a node's prefix (The "Lazy" Cleanup)
    // If Node has prefix "Apple" and we insert "Apply", we split at 'l'.
    void split_node(std::shared_ptr<Node> node, size_t split_point) {
        // 1. Create a new "middle" node that will inherit the original children
        auto new_child = std::make_shared<Node4>();

        // 2. Move the suffix ("e") to the new child
        // Token at split_point is the "Edge" key
        TokenId split_edge_key = node->prefix[split_point];

        // The rest of the prefix goes into the new child
        if (split_point + 1 < node->prefix.size()) {
            new_child->prefix.assign(
                node->prefix.begin() + split_point + 1,
                node->prefix.end()
            );
        }

        // 3. Move existing children/values to new child
        // (If this were a full implementation, we'd copy children vectors here)
        new_child->leaf_value = node->leaf_value;
        node->leaf_value = std::nullopt; // Parent is no longer a leaf

        // 4. Update Parent (The "Stub")
        // Parent now only has the prefix up to split point ("App")
        node->prefix.resize(split_point);

        // 5. Connect Parent -> New Child
        // Using "naive" add_child for now. In reality, we must clear parent's children first.
        // For v0.1 skeleton, we assume we are converting a "Leaf w/ Prefix" to "Inner Node".
        if (node->type == NodeType::Node4) {
             auto* n4 = static_cast<Node4*>(node.get());
             n4->keys.clear();
             n4->children.clear();
             add_child(node, split_edge_key, new_child);
        }
    }

    std::optional<BackendId> lookup_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens) {
        // 1. Check Prefix Match (Optimistic)
        size_t match_len = 0;
        while(match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) return std::nullopt; // Divergence = No Match
            match_len++;
        }

        // If the INPUT ran out before the NODE prefix finished, we didn't match this node.
        // Example: Tree has "Apple", Input is "App". No match.
        if (match_len < node->prefix.size()) return std::nullopt;

        // We have matched THIS node completely.
        auto remaining = tokens.subspan(match_len);

        // 2. Try to go deeper (Greedy Match)
        if (!remaining.empty()) {
            TokenId next_token = remaining[0];
            auto child = find_child(node, next_token);

            if (child) {
                // Recursively look for a LONGER match
                auto result = lookup_recursive(child, remaining.subspan(1));

                // If the child found a valid match, return it.
                if (result.has_value()) {
                    return result;
                }
            }
        }

        // 3. Fallback: If we couldn't go deeper (or the child failed),
        // THIS node is the Longest Matching Prefix.
        return node->leaf_value;
    }
    /*
    std::optional<BackendId> lookup_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens) {
        // 1. Check Prefix Match (Optimistic)
        size_t match_len = 0;
        while(match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) return std::nullopt; // Mismatch
            match_len++;
        }

        // If input ran out, but node has more prefix -> No match
        if (match_len < node->prefix.size()) return std::nullopt;

        auto remaining = tokens.subspan(match_len);

        // Exact Match?
        if (remaining.empty()) {
            return node->leaf_value;
        }

        // Traverse Down
        TokenId next_token = remaining[0];
        auto child = find_child(node, next_token);
        if (child) {
            return lookup_recursive(child, remaining.subspan(1));
        }

        return std::nullopt;
    }
    */
};

} // namespace ranvier
