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
//struct Node256 : public Node {
    // A hash map or direct array could go here.
    // For v0.1, we can just use a larger vector or map.
    // In production ART, this is usually `Node* children[256]` mapped by byte.
    // Since tokens are 32-bit, we might use a std::map for the "Root" mostly.
    // (Implementation deferred for v0.1)
//};
struct Node256 : public Node {
    // Direct mapping: children[token_byte] -> Node*
    // We use std::array for safety, initialized to nullptr
    std::array<std::shared_ptr<Node>, 256> children;

    Node256() : Node(NodeType::Node256) {
        children.fill(nullptr);
    }
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
        root = insert_recursive(root, tokens, backend);
    }

    // LOOKUP
    // Returns the BackendId if found, or nullopt
    std::optional<BackendId> lookup(std::span<const TokenId> tokens) {
        return lookup_recursive(root, tokens);
    }

private:
    std::shared_ptr<Node> root;

    // Recursive Insert Logic (The "Hard" Part)
    // Returns the (possibly new) node to handle node growth/replacement
    std::shared_ptr<Node> insert_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens, BackendId backend) {
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
            return node;
        }

        // CASE C: Need to traverse to a child
        TokenId next_token = remaining_tokens[0];
        auto child = find_child(node, next_token);

        if (child) {
            // Recurse down and update child pointer in case it was replaced
            auto new_child = insert_recursive(child, remaining_tokens.subspan(1), backend);
            if (new_child != child) {
                set_child(node, next_token, new_child);
            }
        } else {
            // Create new child (Lazy Expansion)
            auto new_child = std::make_shared<Node4>();

            // Store the REST of the tokens in the new child's prefix (Lazy)
            if (remaining_tokens.size() > 1) {
                auto suffix = remaining_tokens.subspan(1);
                new_child->prefix.assign(suffix.begin(), suffix.end());
            }

            new_child->leaf_value = backend;
            node = add_child(node, next_token, new_child);
        }
        return node;
    }

    // Helper: Find child by TokenID
    std::shared_ptr<Node> find_child(std::shared_ptr<Node> node, TokenId key) {
        if (node->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(node.get());
            for (size_t i = 0; i < n4->keys.size(); i++) {
                if (n4->keys[i] == key) return n4->children[i];
            }
        } else if (node->type == NodeType::Node256) {
            auto* n256 = static_cast<Node256*>(node.get());
            // Direct lookup using lower 8 bits of key as index
            uint8_t index = static_cast<uint8_t>(key);
            return n256->children[index];
        }
        return nullptr;
    }

    // Helper: Set/update a child pointer in a node
    void set_child(std::shared_ptr<Node> node, TokenId key, std::shared_ptr<Node> child) {
        if (node->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(node.get());
            for (size_t i = 0; i < n4->keys.size(); i++) {
                if (n4->keys[i] == key) {
                    n4->children[i] = child;
                    return;
                }
            }
        } else if (node->type == NodeType::Node256) {
            auto* n256 = static_cast<Node256*>(node.get());
            n256->children[static_cast<uint8_t>(key)] = child;
        }
    }

    // Helper: Add child to node, returns (possibly new) parent if growth occurred
    std::shared_ptr<Node> add_child(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
        // 1. Check if Node4 is full
        if (parent->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(parent.get());
            if (n4->keys.size() < 4) {
                n4->keys.push_back(key);
                n4->children.push_back(child);
                return parent;
            }

            // 2. GROW: Node4 -> Node256 (Skipping 16/48 for brevity)
            auto n256 = std::make_shared<Node256>();
            // Copy existing prefix/value
            n256->prefix = n4->prefix;
            n256->leaf_value = n4->leaf_value;

            // Migrate existing children
            for(size_t i=0; i<n4->keys.size(); i++) {
                // Map the key (byte) to the array index
                // Note: In real ART, we cast TokenId to uint8_t
                uint8_t index = static_cast<uint8_t>(n4->keys[i]);
                n256->children[index] = n4->children[i];
            }

            // Add the NEW child
            n256->children[static_cast<uint8_t>(key)] = child;

            // Return the new node (caller must update their pointer)
            return n256;
        }
        else if (parent->type == NodeType::Node256) {
            auto* n256 = static_cast<Node256*>(parent.get());
            n256->children[static_cast<uint8_t>(key)] = child;
        }
        return parent;
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
        new_child->leaf_value = node->leaf_value;
        node->leaf_value = std::nullopt; // Parent is no longer a leaf

        // Move existing children from node to new_child (for Node4)
        if (node->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(node.get());
            auto* new_n4 = static_cast<Node4*>(new_child.get());
            new_n4->keys = std::move(n4->keys);
            new_n4->children = std::move(n4->children);
            n4->keys.clear();
            n4->children.clear();
        }

        // 4. Update Parent (The "Stub")
        // Parent now only has the prefix up to split point ("App")
        node->prefix.resize(split_point);

        // 5. Connect Parent -> New Child
        if (node->type == NodeType::Node4) {
            auto* n4 = static_cast<Node4*>(node.get());
            n4->keys.push_back(split_edge_key);
            n4->children.push_back(new_child);
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
