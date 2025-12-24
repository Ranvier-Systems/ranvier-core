#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <array>
#include <memory>
#include <span>
#include <algorithm>
#include <chrono>
#include <functional>

#include "types.hpp"

/*
Adaptive Radix Tree (ART) Implementation

Design Philosophy:
- Path Compression: Every node stores a prefix vector to skip checking individual nodes
- Adaptive Nodes: Node4 → Node16 → Node48 → Node256 based on child count
- Zero-Copy Lookups: Uses std::span<int32_t> for the lookup key

Node Types:
- Node4:   1-4 children,   linear search through keys vector
- Node16:  5-16 children,  linear search (SIMD-friendly in production)
- Node48:  17-48 children, 256-byte index array → 48 child pointers
- Node256: 49-256 children, direct 256-element array
*/

namespace ranvier {

// Route origin for distributed mode
// LOCAL routes are learned from direct requests
// REMOTE routes are learned from cluster gossip
enum class RouteOrigin : uint8_t {
    LOCAL = 0,   // Learned from direct request on this node
    REMOTE = 1   // Learned from cluster gossip (can be evicted more aggressively)
};

// Node Types for Adaptive Radix Tree
enum class NodeType : uint8_t {
    Node4,
    Node16,
    Node48,
    Node256
};

// Base Node (Header) - shared by all node types
struct Node {
    NodeType type;

    // Optimistic Path Compression
    // Instead of a chain of nodes A->B->C, we store [A,B,C] here.
    std::vector<TokenId> prefix;

    // Leaf Data: If this node terminates a valid route, this has a value.
    std::optional<BackendId> leaf_value;

    // Route origin: LOCAL (direct request) or REMOTE (cluster gossip)
    // REMOTE routes can be evicted more aggressively than LOCAL routes
    RouteOrigin origin = RouteOrigin::LOCAL;

    // LRU tracking: timestamp of last access (for TTL and eviction)
    // This is shard-local so no atomics needed (Seastar's shared-nothing model)
    std::chrono::steady_clock::time_point last_accessed;

    Node(NodeType t) : type(t), last_accessed(std::chrono::steady_clock::now()) {}
    virtual ~Node() = default;
};

// Node4: Smallest node (1-4 children)
// Linear search through keys, most memory efficient for sparse branching
struct Node4 : public Node {
    std::vector<TokenId> keys;
    std::vector<std::shared_ptr<Node>> children;

    Node4() : Node(NodeType::Node4) {
        keys.reserve(4);
        children.reserve(4);
    }
};

// Node16: Medium-small node (5-16 children)
// Linear search, but SIMD-friendly layout for production optimization
struct Node16 : public Node {
    std::vector<TokenId> keys;
    std::vector<std::shared_ptr<Node>> children;

    Node16() : Node(NodeType::Node16) {
        keys.reserve(16);
        children.reserve(16);
    }
};

// Node48: Medium-large node (17-48 children)
// Uses 256-byte index array for O(1) key lookup → index into children array
// index[key] = 255 means empty, otherwise index[key] = position in children
struct Node48 : public Node {
    static constexpr uint8_t EMPTY_MARKER = 255;

    std::array<uint8_t, 256> index;          // Maps key byte → child position
    std::vector<std::shared_ptr<Node>> children;  // Up to 48 children
    std::vector<TokenId> keys;               // Track original keys for iteration

    Node48() : Node(NodeType::Node48) {
        index.fill(EMPTY_MARKER);
        children.reserve(48);
        keys.reserve(48);
    }
};

// Node256: Largest node (49-256 children)
// Direct array lookup, O(1) access but uses most memory
struct Node256 : public Node {
    std::array<std::shared_ptr<Node>, 256> children;

    Node256() : Node(NodeType::Node256) {
        children.fill(nullptr);
    }
};

// Entry info for LRU management
struct RouteEntry {
    std::vector<TokenId> tokens;
    BackendId backend;
    RouteOrigin origin;
    std::chrono::steady_clock::time_point last_accessed;
};

// The Tree Manager
class RadixTree {
public:
    explicit RadixTree(uint32_t block_alignment = 16)
        : block_alignment_(block_alignment) {
        root = std::make_shared<Node4>();
    }

    // INSERT - O(k) where k is the number of tokens
    // Tokens are truncated to block_alignment boundary for vLLM PagedAttention compatibility
    // origin: LOCAL for direct requests, REMOTE for cluster gossip
    void insert(std::span<const TokenId> tokens, BackendId backend,
                RouteOrigin origin = RouteOrigin::LOCAL) {
        // Align to block boundary - only insert at multiples of block_alignment
        size_t aligned_len = (tokens.size() / block_alignment_) * block_alignment_;
        if (aligned_len == 0) {
            return;  // Not enough tokens for a full block
        }
        auto aligned_tokens = tokens.subspan(0, aligned_len);
        root = insert_recursive(root, aligned_tokens, backend, origin);
        route_count_++;
    }

    // LOOKUP - Returns the BackendId if found, or nullopt
    std::optional<BackendId> lookup(std::span<const TokenId> tokens) {
        return lookup_recursive(root, tokens);
    }

    // Get approximate route count (may be slightly inaccurate due to overwrites)
    size_t route_count() const {
        return route_count_;
    }

    // Traverse all leaf nodes and call callback with entry info
    // Callback signature: void(const RouteEntry&)
    template<typename Callback>
    void for_each_leaf(Callback&& callback) const {
        std::vector<TokenId> path;
        for_each_leaf_recursive(root, path, std::forward<Callback>(callback));
    }

    // Remove routes older than the given time point
    // Returns the number of routes removed
    size_t remove_expired(std::chrono::steady_clock::time_point cutoff) {
        size_t removed = 0;
        remove_expired_recursive(root, cutoff, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

    // Evict the oldest route (for LRU eviction when at capacity)
    // Returns true if a route was evicted
    bool evict_oldest() {
        if (find_and_remove_oldest(root)) {
            if (route_count_ > 0) route_count_--;
            return true;
        }
        return false;
    }

    // Evict the oldest REMOTE route first (more aggressive eviction for gossip-learned routes)
    // Falls back to evicting any oldest route if no REMOTE routes exist
    // Returns true if a route was evicted
    bool evict_oldest_remote() {
        // First try to evict oldest remote route
        if (find_and_remove_oldest_by_origin(root, RouteOrigin::REMOTE)) {
            if (route_count_ > 0) route_count_--;
            return true;
        }
        // Fall back to evicting any oldest route
        return evict_oldest();
    }

    // Remove all routes matching a specific backend and origin
    // Used for cluster-wide route pruning when a peer fails
    // Returns the number of routes removed
    size_t remove_routes_by_backend(BackendId backend, RouteOrigin origin) {
        size_t removed = 0;
        remove_routes_by_backend_recursive(root, backend, origin, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

private:
    std::shared_ptr<Node> root;
    uint32_t block_alignment_;  // vLLM PagedAttention block size
    size_t route_count_ = 0;

    // Get the byte index for a token (lower 8 bits)
    static uint8_t key_byte(TokenId key) {
        return static_cast<uint8_t>(key);
    }

    // Recursive Insert Logic
    std::shared_ptr<Node> insert_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens,
                                           BackendId backend, RouteOrigin origin) {
        // 1. Calculate matching prefix length
        size_t match_len = 0;
        while (match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) break;
            match_len++;
        }

        // CASE A: Prefix mismatch - need to split
        if (match_len < node->prefix.size()) {
            split_node(node, match_len);
        }

        auto remaining_tokens = tokens.subspan(match_len);

        // CASE B: Exact match - set leaf value
        if (remaining_tokens.empty()) {
            node->leaf_value = backend;
            node->origin = origin;
            node->last_accessed = std::chrono::steady_clock::now();
            return node;
        }

        // CASE C: Need to traverse to a child
        TokenId next_token = remaining_tokens[0];
        auto child = find_child(node, next_token);

        if (child) {
            auto new_child = insert_recursive(child, remaining_tokens.subspan(1), backend, origin);
            if (new_child != child) {
                set_child(node, next_token, new_child);
            }
        } else {
            auto new_child = std::make_shared<Node4>();
            if (remaining_tokens.size() > 1) {
                auto suffix = remaining_tokens.subspan(1);
                new_child->prefix.assign(suffix.begin(), suffix.end());
            }
            new_child->leaf_value = backend;
            new_child->origin = origin;
            node = add_child(node, next_token, new_child);
        }
        return node;
    }

    // Find child by TokenID
    std::shared_ptr<Node> find_child(std::shared_ptr<Node> node, TokenId key) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return n->children[i];
                }
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return n->children[i];
                }
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                uint8_t idx = n->index[key_byte(key)];
                if (idx != Node48::EMPTY_MARKER) {
                    return n->children[idx];
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                return n->children[key_byte(key)];
            }
        }
        return nullptr;
    }

    // Set/update a child pointer
    void set_child(std::shared_ptr<Node> node, TokenId key, std::shared_ptr<Node> child) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) {
                        n->children[i] = child;
                        return;
                    }
                }
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) {
                        n->children[i] = child;
                        return;
                    }
                }
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                uint8_t idx = n->index[key_byte(key)];
                if (idx != Node48::EMPTY_MARKER) {
                    n->children[idx] = child;
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                n->children[key_byte(key)] = child;
                break;
            }
        }
    }

    // Add child with automatic node growth
    std::shared_ptr<Node> add_child(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
        switch (parent->type) {
            case NodeType::Node4: {
                auto* n4 = static_cast<Node4*>(parent.get());
                if (n4->keys.size() < 4) {
                    n4->keys.push_back(key);
                    n4->children.push_back(child);
                    return parent;
                }
                // Grow: Node4 → Node16
                return grow_node4_to_node16(parent, key, child);
            }
            case NodeType::Node16: {
                auto* n16 = static_cast<Node16*>(parent.get());
                if (n16->keys.size() < 16) {
                    n16->keys.push_back(key);
                    n16->children.push_back(child);
                    return parent;
                }
                // Grow: Node16 → Node48
                return grow_node16_to_node48(parent, key, child);
            }
            case NodeType::Node48: {
                auto* n48 = static_cast<Node48*>(parent.get());
                if (n48->children.size() < 48) {
                    uint8_t pos = static_cast<uint8_t>(n48->children.size());
                    n48->index[key_byte(key)] = pos;
                    n48->children.push_back(child);
                    n48->keys.push_back(key);
                    return parent;
                }
                // Grow: Node48 → Node256
                return grow_node48_to_node256(parent, key, child);
            }
            case NodeType::Node256: {
                auto* n256 = static_cast<Node256*>(parent.get());
                n256->children[key_byte(key)] = child;
                return parent;
            }
        }
        return parent;
    }

    // Growth functions
    std::shared_ptr<Node> grow_node4_to_node16(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
        auto* n4 = static_cast<Node4*>(parent.get());
        auto n16 = std::make_shared<Node16>();

        // Copy metadata
        n16->prefix = std::move(n4->prefix);
        n16->leaf_value = n4->leaf_value;

        // Migrate existing children
        n16->keys = std::move(n4->keys);
        n16->children = std::move(n4->children);

        // Add new child
        n16->keys.push_back(key);
        n16->children.push_back(child);

        return n16;
    }

    std::shared_ptr<Node> grow_node16_to_node48(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
        auto* n16 = static_cast<Node16*>(parent.get());
        auto n48 = std::make_shared<Node48>();

        // Copy metadata
        n48->prefix = std::move(n16->prefix);
        n48->leaf_value = n16->leaf_value;

        // Migrate existing children
        for (size_t i = 0; i < n16->keys.size(); i++) {
            n48->index[key_byte(n16->keys[i])] = static_cast<uint8_t>(i);
            n48->children.push_back(n16->children[i]);
            n48->keys.push_back(n16->keys[i]);
        }

        // Add new child
        uint8_t pos = static_cast<uint8_t>(n48->children.size());
        n48->index[key_byte(key)] = pos;
        n48->children.push_back(child);
        n48->keys.push_back(key);

        return n48;
    }

    std::shared_ptr<Node> grow_node48_to_node256(std::shared_ptr<Node> parent, TokenId key, std::shared_ptr<Node> child) {
        auto* n48 = static_cast<Node48*>(parent.get());
        auto n256 = std::make_shared<Node256>();

        // Copy metadata
        n256->prefix = std::move(n48->prefix);
        n256->leaf_value = n48->leaf_value;

        // Migrate existing children using the index array
        for (size_t i = 0; i < n48->keys.size(); i++) {
            n256->children[key_byte(n48->keys[i])] = n48->children[i];
        }

        // Add new child
        n256->children[key_byte(key)] = child;

        return n256;
    }

    // Split a node's prefix
    void split_node(std::shared_ptr<Node> node, size_t split_point) {
        auto new_child = std::make_shared<Node4>();

        TokenId split_edge_key = node->prefix[split_point];

        // Move suffix to new child
        if (split_point + 1 < node->prefix.size()) {
            new_child->prefix.assign(
                node->prefix.begin() + split_point + 1,
                node->prefix.end()
            );
        }

        // Move leaf value and children to new child
        new_child->leaf_value = node->leaf_value;
        node->leaf_value = std::nullopt;

        // Move children based on node type
        switch (node->type) {
            case NodeType::Node4: {
                auto* n4 = static_cast<Node4*>(node.get());
                auto* new_n4 = static_cast<Node4*>(new_child.get());
                new_n4->keys = std::move(n4->keys);
                new_n4->children = std::move(n4->children);
                n4->keys.clear();
                n4->children.clear();
                break;
            }
            case NodeType::Node16: {
                auto* n16 = static_cast<Node16*>(node.get());
                auto* new_n4 = static_cast<Node4*>(new_child.get());
                // Node16 becomes Node4 after split (only 1 child)
                new_n4->keys = std::move(n16->keys);
                new_n4->children = std::move(n16->children);
                n16->keys.clear();
                n16->children.clear();
                break;
            }
            case NodeType::Node48: {
                auto* n48 = static_cast<Node48*>(node.get());
                auto* new_n4 = static_cast<Node4*>(new_child.get());
                new_n4->keys = std::move(n48->keys);
                new_n4->children = std::move(n48->children);
                n48->index.fill(Node48::EMPTY_MARKER);
                n48->keys.clear();
                n48->children.clear();
                break;
            }
            case NodeType::Node256: {
                // For Node256 split, we need to collect all children
                auto* n256 = static_cast<Node256*>(node.get());
                auto* new_n4 = static_cast<Node4*>(new_child.get());
                for (int i = 0; i < 256; i++) {
                    if (n256->children[i]) {
                        new_n4->keys.push_back(static_cast<TokenId>(i));
                        new_n4->children.push_back(n256->children[i]);
                        n256->children[i] = nullptr;
                    }
                }
                break;
            }
        }

        // Truncate parent prefix
        node->prefix.resize(split_point);

        // Add new_child to parent (parent now has only 1 child after split)
        switch (node->type) {
            case NodeType::Node4: {
                auto* n4 = static_cast<Node4*>(node.get());
                n4->keys.push_back(split_edge_key);
                n4->children.push_back(new_child);
                break;
            }
            case NodeType::Node16: {
                auto* n16 = static_cast<Node16*>(node.get());
                n16->keys.push_back(split_edge_key);
                n16->children.push_back(new_child);
                break;
            }
            case NodeType::Node48: {
                auto* n48 = static_cast<Node48*>(node.get());
                n48->index[key_byte(split_edge_key)] = 0;
                n48->children.push_back(new_child);
                n48->keys.push_back(split_edge_key);
                break;
            }
            case NodeType::Node256: {
                auto* n256 = static_cast<Node256*>(node.get());
                n256->children[key_byte(split_edge_key)] = new_child;
                break;
            }
        }
    }

    // Recursive lookup
    std::optional<BackendId> lookup_recursive(std::shared_ptr<Node> node, std::span<const TokenId> tokens) {
        // Check prefix match
        size_t match_len = 0;
        while (match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) return std::nullopt;
            match_len++;
        }

        // Input shorter than prefix - no match
        if (match_len < node->prefix.size()) return std::nullopt;

        auto remaining = tokens.subspan(match_len);

        // Try to go deeper
        if (!remaining.empty()) {
            TokenId next_token = remaining[0];
            auto child = find_child(node, next_token);

            if (child) {
                auto result = lookup_recursive(child, remaining.subspan(1));
                if (result.has_value()) {
                    // Touch timestamp on the matched child (LRU tracking)
                    child->last_accessed = std::chrono::steady_clock::now();
                    return result;
                }
            }
        }

        // Touch timestamp if this node has a leaf value (LRU tracking)
        if (node->leaf_value.has_value()) {
            node->last_accessed = std::chrono::steady_clock::now();
        }

        // Return this node's value (longest prefix match)
        return node->leaf_value;
    }

    // Helper: Traverse all leaves recursively
    template<typename Callback>
    void for_each_leaf_recursive(std::shared_ptr<Node> node, std::vector<TokenId>& path, Callback&& callback) const {
        if (!node) return;

        // Add this node's prefix to path
        size_t prefix_start = path.size();
        path.insert(path.end(), node->prefix.begin(), node->prefix.end());

        // If this node has a leaf value, call the callback
        if (node->leaf_value.has_value()) {
            RouteEntry entry;
            entry.tokens = path;
            entry.backend = node->leaf_value.value();
            entry.origin = node->origin;
            entry.last_accessed = node->last_accessed;
            callback(entry);
        }

        // Traverse children
        auto visit_children = [&](auto& keys, auto& children) {
            for (size_t i = 0; i < keys.size(); i++) {
                path.push_back(keys[i]);
                for_each_leaf_recursive(children[i], path, callback);
                path.pop_back();
            }
        };

        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                visit_children(n->keys, n->children);
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                visit_children(n->keys, n->children);
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                visit_children(n->keys, n->children);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) {
                        path.push_back(static_cast<TokenId>(i));
                        for_each_leaf_recursive(n->children[i], path, callback);
                        path.pop_back();
                    }
                }
                break;
            }
        }

        // Remove this node's prefix from path
        path.resize(prefix_start);
    }

    // Helper: Remove expired leaf values recursively
    void remove_expired_recursive(std::shared_ptr<Node> node, std::chrono::steady_clock::time_point cutoff, size_t& removed) {
        if (!node) return;

        // Check if this node's leaf value is expired
        if (node->leaf_value.has_value() && node->last_accessed < cutoff) {
            node->leaf_value = std::nullopt;
            removed++;
        }

        // Traverse children
        auto visit_children = [&](auto& children) {
            for (auto& child : children) {
                if (child) {
                    remove_expired_recursive(child, cutoff, removed);
                }
            }
        };

        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                visit_children(n->children);
                break;
            }
        }
    }

    // Helper: Find and remove the oldest leaf value
    // Returns true if a leaf was removed
    bool find_and_remove_oldest(std::shared_ptr<Node> node) {
        if (!node) return false;

        std::shared_ptr<Node> oldest_node = nullptr;
        std::chrono::steady_clock::time_point oldest_time = std::chrono::steady_clock::time_point::max();

        // Find the oldest leaf
        find_oldest_recursive(node, oldest_node, oldest_time);

        // Remove the oldest leaf's value
        if (oldest_node && oldest_node->leaf_value.has_value()) {
            oldest_node->leaf_value = std::nullopt;
            return true;
        }
        return false;
    }

    // Helper: Find and remove the oldest leaf value with specific origin
    // Returns true if a leaf was removed
    bool find_and_remove_oldest_by_origin(std::shared_ptr<Node> node, RouteOrigin target_origin) {
        if (!node) return false;

        std::shared_ptr<Node> oldest_node = nullptr;
        std::chrono::steady_clock::time_point oldest_time = std::chrono::steady_clock::time_point::max();

        // Find the oldest leaf with matching origin
        find_oldest_by_origin_recursive(node, oldest_node, oldest_time, target_origin);

        // Remove the oldest leaf's value
        if (oldest_node && oldest_node->leaf_value.has_value()) {
            oldest_node->leaf_value = std::nullopt;
            return true;
        }
        return false;
    }

    // Helper: Find the oldest leaf node
    void find_oldest_recursive(std::shared_ptr<Node> node, std::shared_ptr<Node>& oldest_node,
                               std::chrono::steady_clock::time_point& oldest_time) {
        if (!node) return;

        // Check if this node has a leaf value and is older than current oldest
        if (node->leaf_value.has_value() && node->last_accessed < oldest_time) {
            oldest_time = node->last_accessed;
            oldest_node = node;
        }

        // Traverse children
        auto visit_children = [&](auto& children) {
            for (auto& child : children) {
                if (child) {
                    find_oldest_recursive(child, oldest_node, oldest_time);
                }
            }
        };

        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                visit_children(n->children);
                break;
            }
        }
    }

    // Helper: Find the oldest leaf node with specific origin
    void find_oldest_by_origin_recursive(std::shared_ptr<Node> node, std::shared_ptr<Node>& oldest_node,
                                         std::chrono::steady_clock::time_point& oldest_time,
                                         RouteOrigin target_origin) {
        if (!node) return;

        // Check if this node has a leaf value with matching origin and is older than current oldest
        if (node->leaf_value.has_value() &&
            node->origin == target_origin &&
            node->last_accessed < oldest_time) {
            oldest_time = node->last_accessed;
            oldest_node = node;
        }

        // Traverse children
        auto visit_children = [&](auto& children) {
            for (auto& child : children) {
                if (child) {
                    find_oldest_by_origin_recursive(child, oldest_node, oldest_time, target_origin);
                }
            }
        };

        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                visit_children(n->children);
                break;
            }
        }
    }

    // Helper: Remove routes matching a specific backend and origin
    void remove_routes_by_backend_recursive(std::shared_ptr<Node> node, BackendId backend,
                                            RouteOrigin target_origin, size_t& removed) {
        if (!node) return;

        // Check if this node has a leaf value matching both backend and origin
        if (node->leaf_value.has_value() &&
            node->leaf_value.value() == backend &&
            node->origin == target_origin) {
            node->leaf_value = std::nullopt;
            removed++;
        }

        // Traverse children
        auto visit_children = [&](auto& children) {
            for (auto& child : children) {
                if (child) {
                    remove_routes_by_backend_recursive(child, backend, target_origin, removed);
                }
            }
        };

        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node.get());
                visit_children(n->children);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node.get());
                visit_children(n->children);
                break;
            }
        }
    }
};

} // namespace ranvier
