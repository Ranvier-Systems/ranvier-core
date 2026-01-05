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
- Unique Ownership: Uses std::unique_ptr for node ownership (Seastar shared-nothing model)

Node Types:
- Node4:   1-4 children,   linear search through keys vector
- Node16:  5-16 children,  linear search (SIMD-friendly in production)
- Node48:  17-48 children, 256-byte index array → 48 child pointers
- Node256: 49-256 children, direct 256-element array

Memory Model:
- RadixTree instances are thread-local (owned uniquely by RouterService on each shard)
- No cross-thread sharing, so std::unique_ptr is used instead of std::shared_ptr
- This eliminates atomic reference counting overhead (lock xadd instructions)
*/

namespace ranvier {

// =============================================================================
// Types and Enums
// =============================================================================

enum class RouteOrigin : uint8_t {
    LOCAL = 0,   // Learned from direct request on this node
    REMOTE = 1   // Learned from cluster gossip (can be evicted more aggressively)
};

enum class NodeType : uint8_t {
    Node4,
    Node16,
    Node48,
    Node256
};

// =============================================================================
// Node Structures
// =============================================================================

struct Node {
    NodeType type;
    std::vector<TokenId> prefix;
    std::optional<BackendId> leaf_value;
    RouteOrigin origin = RouteOrigin::LOCAL;
    std::chrono::steady_clock::time_point last_accessed;

    explicit Node(NodeType t) : type(t), last_accessed(std::chrono::steady_clock::now()) {}
    virtual ~Node() = default;
};

struct Node4 : public Node {
    std::vector<TokenId> keys;
    std::vector<std::unique_ptr<Node>> children;

    Node4() : Node(NodeType::Node4) {
        keys.reserve(4);
        children.reserve(4);
    }
};

struct Node16 : public Node {
    std::vector<TokenId> keys;
    std::vector<std::unique_ptr<Node>> children;

    Node16() : Node(NodeType::Node16) {
        keys.reserve(16);
        children.reserve(16);
    }
};

struct Node48 : public Node {
    static constexpr uint8_t EMPTY_MARKER = 255;

    std::array<uint8_t, 256> index;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<TokenId> keys;

    Node48() : Node(NodeType::Node48) {
        index.fill(EMPTY_MARKER);
        children.reserve(48);
        keys.reserve(48);
    }
};

struct Node256 : public Node {
    std::array<std::unique_ptr<Node>, 256> children;

    Node256() : Node(NodeType::Node256) {}
};

// =============================================================================
// Route Entry (for iteration)
// =============================================================================

struct RouteEntry {
    std::vector<TokenId> tokens;
    BackendId backend;
    RouteOrigin origin;
    std::chrono::steady_clock::time_point last_accessed;
};

// =============================================================================
// RadixTree Class
// =============================================================================

class RadixTree {
public:
    explicit RadixTree(uint32_t block_alignment = 16)
        : block_alignment_(block_alignment)
        , root_(std::make_unique<Node4>()) {}

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void insert(std::span<const TokenId> tokens, BackendId backend,
                RouteOrigin origin = RouteOrigin::LOCAL) {
        size_t aligned_len = (tokens.size() / block_alignment_) * block_alignment_;
        if (aligned_len == 0) return;

        root_ = insert_recursive(std::move(root_), tokens.subspan(0, aligned_len), backend, origin);
        route_count_++;
    }

    std::optional<BackendId> lookup(std::span<const TokenId> tokens) {
        return lookup_recursive(root_.get(), tokens);
    }

    size_t route_count() const { return route_count_; }

    template<typename Callback>
    void for_each_leaf(Callback&& callback) const {
        std::vector<TokenId> path;
        for_each_leaf_recursive(root_.get(), path, std::forward<Callback>(callback));
    }

    size_t remove_expired(std::chrono::steady_clock::time_point cutoff) {
        size_t removed = 0;
        remove_expired_recursive(root_.get(), cutoff, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

    bool evict_oldest() {
        Node* oldest = find_oldest_leaf(root_.get(), std::nullopt);
        if (oldest && oldest->leaf_value.has_value()) {
            oldest->leaf_value = std::nullopt;
            if (route_count_ > 0) route_count_--;
            return true;
        }
        return false;
    }

    bool evict_oldest_remote() {
        Node* oldest = find_oldest_leaf(root_.get(), RouteOrigin::REMOTE);
        if (oldest && oldest->leaf_value.has_value()) {
            oldest->leaf_value = std::nullopt;
            if (route_count_ > 0) route_count_--;
            return true;
        }
        return evict_oldest();
    }

    size_t remove_routes_by_backend(BackendId backend, RouteOrigin origin) {
        size_t removed = 0;
        remove_by_backend_recursive(root_.get(), backend, origin, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

private:
    std::unique_ptr<Node> root_;
    uint32_t block_alignment_;
    size_t route_count_ = 0;

    // -------------------------------------------------------------------------
    // Node Visitor Helpers
    // -------------------------------------------------------------------------

    // Visit all children of a node with a callback: void(TokenId key, Node* child)
    template<typename Callback>
    static void visit_children(Node* node, Callback&& callback) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    callback(n->keys[i], n->children[i].get());
                }
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    callback(n->keys[i], n->children[i].get());
                }
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    callback(n->keys[i], n->children[i].get());
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) {
                        callback(static_cast<TokenId>(i), n->children[i].get());
                    }
                }
                break;
            }
        }
    }

    // Get the number of children in a node
    static size_t child_count(Node* node) {
        switch (node->type) {
            case NodeType::Node4:
                return static_cast<Node4*>(node)->keys.size();
            case NodeType::Node16:
                return static_cast<Node16*>(node)->keys.size();
            case NodeType::Node48:
                return static_cast<Node48*>(node)->keys.size();
            case NodeType::Node256: {
                size_t count = 0;
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) count++;
                }
                return count;
            }
        }
        return 0;
    }

    // Get lower 8 bits of token for array indexing
    static uint8_t key_byte(TokenId key) {
        return static_cast<uint8_t>(key);
    }

    // -------------------------------------------------------------------------
    // Child Access Operations
    // -------------------------------------------------------------------------

    // Find child by key - hot path, optimized for common cases
    Node* find_child(Node* node, TokenId key) const {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node);
                // Small node: linear scan is cache-friendly
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return n->children[i].get();
                }
                return nullptr;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node);
                // Medium node: linear scan, SIMD-friendly in future
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return n->children[i].get();
                }
                return nullptr;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                // O(1) index lookup
                uint8_t idx = n->index[key_byte(key)];
                if (idx != Node48::EMPTY_MARKER) [[likely]] {
                    return n->children[idx].get();
                }
                return nullptr;
            }
            case NodeType::Node256: {
                // O(1) direct array access - most common for dense nodes
                return static_cast<Node256*>(node)->children[key_byte(key)].get();
            }
        }
        return nullptr;
    }

    std::unique_ptr<Node> extract_child(Node* node, TokenId key) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return std::move(n->children[i]);
                }
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) return std::move(n->children[i]);
                }
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                uint8_t idx = n->index[key_byte(key)];
                if (idx != Node48::EMPTY_MARKER) {
                    return std::move(n->children[idx]);
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                return std::move(n->children[key_byte(key)]);
            }
        }
        return nullptr;
    }

    void set_child(Node* node, TokenId key, std::unique_ptr<Node> child) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) {
                        n->children[i] = std::move(child);
                        return;
                    }
                }
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node);
                for (size_t i = 0; i < n->keys.size(); i++) {
                    if (n->keys[i] == key) {
                        n->children[i] = std::move(child);
                        return;
                    }
                }
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                uint8_t idx = n->index[key_byte(key)];
                if (idx != Node48::EMPTY_MARKER) {
                    n->children[idx] = std::move(child);
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                n->children[key_byte(key)] = std::move(child);
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Node Growth Operations
    // -------------------------------------------------------------------------

    static void copy_node_metadata(Node* dest, Node* src) {
        dest->prefix = std::move(src->prefix);
        dest->leaf_value = src->leaf_value;
        dest->origin = src->origin;
        dest->last_accessed = src->last_accessed;
    }

    std::unique_ptr<Node> add_child(std::unique_ptr<Node> parent, TokenId key, std::unique_ptr<Node> child) {
        switch (parent->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(parent.get());
                if (n->keys.size() < 4) {
                    n->keys.push_back(key);
                    n->children.push_back(std::move(child));
                    return parent;
                }
                return grow_to_node16(std::move(parent), key, std::move(child));
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(parent.get());
                if (n->keys.size() < 16) {
                    n->keys.push_back(key);
                    n->children.push_back(std::move(child));
                    return parent;
                }
                return grow_to_node48(std::move(parent), key, std::move(child));
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(parent.get());
                if (n->children.size() < 48) {
                    uint8_t pos = static_cast<uint8_t>(n->children.size());
                    n->index[key_byte(key)] = pos;
                    n->children.push_back(std::move(child));
                    n->keys.push_back(key);
                    return parent;
                }
                return grow_to_node256(std::move(parent), key, std::move(child));
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(parent.get());
                n->children[key_byte(key)] = std::move(child);
                return parent;
            }
        }
        return parent;
    }

    std::unique_ptr<Node> grow_to_node16(std::unique_ptr<Node> parent, TokenId key, std::unique_ptr<Node> child) {
        auto* n4 = static_cast<Node4*>(parent.get());
        auto n16 = std::make_unique<Node16>();

        copy_node_metadata(n16.get(), n4);
        n16->keys = std::move(n4->keys);
        n16->children = std::move(n4->children);
        n16->keys.push_back(key);
        n16->children.push_back(std::move(child));

        return n16;
    }

    std::unique_ptr<Node> grow_to_node48(std::unique_ptr<Node> parent, TokenId key, std::unique_ptr<Node> child) {
        auto* n16 = static_cast<Node16*>(parent.get());
        auto n48 = std::make_unique<Node48>();

        copy_node_metadata(n48.get(), n16);
        for (size_t i = 0; i < n16->keys.size(); i++) {
            n48->index[key_byte(n16->keys[i])] = static_cast<uint8_t>(i);
            n48->children.push_back(std::move(n16->children[i]));
            n48->keys.push_back(n16->keys[i]);
        }

        uint8_t pos = static_cast<uint8_t>(n48->children.size());
        n48->index[key_byte(key)] = pos;
        n48->children.push_back(std::move(child));
        n48->keys.push_back(key);

        return n48;
    }

    std::unique_ptr<Node> grow_to_node256(std::unique_ptr<Node> parent, TokenId key, std::unique_ptr<Node> child) {
        auto* n48 = static_cast<Node48*>(parent.get());
        auto n256 = std::make_unique<Node256>();

        copy_node_metadata(n256.get(), n48);
        for (size_t i = 0; i < n48->keys.size(); i++) {
            n256->children[key_byte(n48->keys[i])] = std::move(n48->children[i]);
        }
        n256->children[key_byte(key)] = std::move(child);

        return n256;
    }

    // -------------------------------------------------------------------------
    // Node Creation Helper
    // -------------------------------------------------------------------------

    static std::unique_ptr<Node> create_node_for_capacity(size_t capacity) {
        if (capacity <= 4) return std::make_unique<Node4>();
        if (capacity <= 16) return std::make_unique<Node16>();
        if (capacity <= 48) return std::make_unique<Node48>();
        return std::make_unique<Node256>();
    }

    // -------------------------------------------------------------------------
    // Split Operation
    // -------------------------------------------------------------------------

    void split_node(Node* node, size_t split_point) {
        TokenId edge_key = node->prefix[split_point];
        size_t num_children = child_count(node);

        // Create appropriately-sized child node
        auto new_child = create_node_for_capacity(num_children);

        // Move prefix suffix to child
        if (split_point + 1 < node->prefix.size()) {
            new_child->prefix.assign(
                node->prefix.begin() + split_point + 1,
                node->prefix.end()
            );
        }

        // Transfer leaf data to child
        new_child->leaf_value = node->leaf_value;
        new_child->origin = node->origin;
        new_child->last_accessed = node->last_accessed;
        node->leaf_value = std::nullopt;

        // Move children to new node
        move_children_to_new_node(node, new_child.get());

        // Truncate parent prefix and add single child
        node->prefix.resize(split_point);
        add_single_child_after_split(node, edge_key, std::move(new_child));
    }

    void move_children_to_new_node(Node* src, Node* dest) {
        switch (src->type) {
            case NodeType::Node4:
                move_from_small_node(static_cast<Node4*>(src), dest);
                break;
            case NodeType::Node16:
                move_from_small_node(static_cast<Node16*>(src), dest);
                break;
            case NodeType::Node48:
                move_from_node48(static_cast<Node48*>(src), dest);
                break;
            case NodeType::Node256:
                move_from_node256(static_cast<Node256*>(src), dest);
                break;
        }
    }

    template<typename SmallNode>
    void move_from_small_node(SmallNode* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            default:
                break;  // Should not happen for small source nodes
        }
        src->keys.clear();
        src->children.clear();
    }

    void move_from_node48(Node48* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node48: {
                auto* d = static_cast<Node48*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                d->index = src->index;
                break;
            }
            default:
                break;
        }
        src->index.fill(Node48::EMPTY_MARKER);
        src->keys.clear();
        src->children.clear();
    }

    void move_from_node256(Node256* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        d->keys.push_back(static_cast<TokenId>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        d->keys.push_back(static_cast<TokenId>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node48: {
                auto* d = static_cast<Node48*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        uint8_t pos = static_cast<uint8_t>(d->children.size());
                        d->index[static_cast<uint8_t>(i)] = pos;
                        d->keys.push_back(static_cast<TokenId>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node256: {
                auto* d = static_cast<Node256*>(dest);
                for (int i = 0; i < 256; i++) {
                    d->children[i] = std::move(src->children[i]);
                }
                break;
            }
        }
    }

    void add_single_child_after_split(Node* node, TokenId key, std::unique_ptr<Node> child) {
        switch (node->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(node);
                n->keys.push_back(key);
                n->children.push_back(std::move(child));
                break;
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(node);
                n->keys.push_back(key);
                n->children.push_back(std::move(child));
                break;
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                n->index[key_byte(key)] = 0;
                n->children.push_back(std::move(child));
                n->keys.push_back(key);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                n->children[key_byte(key)] = std::move(child);
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Insert Implementation
    // -------------------------------------------------------------------------

    std::unique_ptr<Node> insert_recursive(std::unique_ptr<Node> node,
                                           std::span<const TokenId> tokens,
                                           BackendId backend,
                                           RouteOrigin origin) {
        // Calculate prefix match length
        size_t match_len = 0;
        while (match_len < node->prefix.size() && match_len < tokens.size()) {
            if (node->prefix[match_len] != tokens[match_len]) break;
            match_len++;
        }

        // Split if prefix mismatch
        if (match_len < node->prefix.size()) {
            split_node(node.get(), match_len);
        }

        auto remaining = tokens.subspan(match_len);

        // Exact match - update leaf
        if (remaining.empty()) {
            node->leaf_value = backend;
            node->origin = origin;
            node->last_accessed = std::chrono::steady_clock::now();
            return node;
        }

        // Traverse or create child
        TokenId next_key = remaining[0];
        if (find_child(node.get(), next_key)) {
            auto child_ptr = extract_child(node.get(), next_key);
            auto new_child = insert_recursive(std::move(child_ptr), remaining.subspan(1), backend, origin);
            set_child(node.get(), next_key, std::move(new_child));
        } else {
            auto new_child = std::make_unique<Node4>();
            if (remaining.size() > 1) {
                new_child->prefix.assign(remaining.begin() + 1, remaining.end());
            }
            new_child->leaf_value = backend;
            new_child->origin = origin;
            node = add_child(std::move(node), next_key, std::move(new_child));
        }

        return node;
    }

    // -------------------------------------------------------------------------
    // Lookup Implementation
    // -------------------------------------------------------------------------

    // Iterative lookup for better performance (no recursion overhead)
    std::optional<BackendId> lookup_recursive(Node* node, std::span<const TokenId> tokens) {
        std::optional<BackendId> best_match = std::nullopt;
        Node* best_match_node = nullptr;

        while (node != nullptr) {
            // Check prefix match
            const auto& prefix = node->prefix;
            size_t prefix_len = prefix.size();
            size_t tokens_len = tokens.size();
            size_t match_len = 0;

            // Fast prefix comparison
            while (match_len < prefix_len && match_len < tokens_len) {
                if (prefix[match_len] != tokens[match_len]) {
                    // Prefix mismatch - return best match so far
                    if (best_match_node) {
                        best_match_node->last_accessed = std::chrono::steady_clock::now();
                    }
                    return best_match;
                }
                match_len++;
            }

            // Input shorter than prefix - return best match
            if (match_len < prefix_len) {
                if (best_match_node) {
                    best_match_node->last_accessed = std::chrono::steady_clock::now();
                }
                return best_match;
            }

            // Update best match if this node has a value
            if (node->leaf_value.has_value()) {
                best_match = node->leaf_value;
                best_match_node = node;
            }

            // Advance past matched prefix
            tokens = tokens.subspan(match_len);

            // If no more tokens, we're done
            if (tokens.empty()) [[unlikely]] {
                break;
            }

            // Find child for next token
            node = find_child(node, tokens[0]);
            tokens = tokens.subspan(1);
        }

        // Touch LRU timestamp for matched node
        if (best_match_node) {
            best_match_node->last_accessed = std::chrono::steady_clock::now();
        }
        return best_match;
    }

    // -------------------------------------------------------------------------
    // Traversal Implementation
    // -------------------------------------------------------------------------

    template<typename Callback>
    void for_each_leaf_recursive(Node* node, std::vector<TokenId>& path, Callback&& callback) const {
        if (!node) return;

        size_t prefix_start = path.size();
        path.insert(path.end(), node->prefix.begin(), node->prefix.end());

        if (node->leaf_value.has_value()) {
            RouteEntry entry{path, node->leaf_value.value(), node->origin, node->last_accessed};
            callback(entry);
        }

        visit_children(node, [&](TokenId key, Node* child) {
            path.push_back(key);
            for_each_leaf_recursive(child, path, callback);
            path.pop_back();
        });

        path.resize(prefix_start);
    }

    // -------------------------------------------------------------------------
    // Eviction Implementation
    // -------------------------------------------------------------------------

    void remove_expired_recursive(Node* node, std::chrono::steady_clock::time_point cutoff, size_t& removed) {
        if (!node) return;

        if (node->leaf_value.has_value() && node->last_accessed < cutoff) {
            node->leaf_value = std::nullopt;
            removed++;
        }

        visit_children(node, [&](TokenId, Node* child) {
            remove_expired_recursive(child, cutoff, removed);
        });
    }

    Node* find_oldest_leaf(Node* node, std::optional<RouteOrigin> filter_origin) {
        if (!node) return nullptr;

        Node* oldest = nullptr;
        auto oldest_time = std::chrono::steady_clock::time_point::max();

        find_oldest_recursive(node, oldest, oldest_time, filter_origin);
        return oldest;
    }

    void find_oldest_recursive(Node* node, Node*& oldest,
                               std::chrono::steady_clock::time_point& oldest_time,
                               std::optional<RouteOrigin> filter_origin) {
        if (!node) return;

        bool matches_filter = !filter_origin.has_value() || node->origin == filter_origin.value();
        if (node->leaf_value.has_value() && matches_filter && node->last_accessed < oldest_time) {
            oldest_time = node->last_accessed;
            oldest = node;
        }

        visit_children(node, [&](TokenId, Node* child) {
            find_oldest_recursive(child, oldest, oldest_time, filter_origin);
        });
    }

    void remove_by_backend_recursive(Node* node, BackendId backend, RouteOrigin origin, size_t& removed) {
        if (!node) return;

        if (node->leaf_value.has_value() &&
            node->leaf_value.value() == backend &&
            node->origin == origin) {
            node->leaf_value = std::nullopt;
            removed++;
        }

        visit_children(node, [&](TokenId, Node* child) {
            remove_by_backend_recursive(child, backend, origin, removed);
        });
    }
};

} // namespace ranvier
