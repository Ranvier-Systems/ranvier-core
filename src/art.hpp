#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <emmintrin.h> // SSE2
#include <cassert>

// Node types
enum class NodeType : uint8_t { Node4, Node16, Node48, Node256 };

// Fixed header for all nodes
struct Node {
    NodeType type;
    uint8_t numChildren;

    // Path compression: optimistic prefix storage
    uint32_t prefixLen = 0;
    uint8_t prefix[8]; // Small buffer for prefix, simplified for example

    Node(NodeType t) : type(t), numChildren(0) {}
};

// 1. Node4: Smallest node. Linear scan.
struct Node4 : public Node {
    uint8_t keys[4];
    Node* children[4];

    Node4() : Node(NodeType::Node4) {
        memset(keys, 0, 4);
        memset(children, 0, sizeof(children));
    }
};

// 2. Node16: Medium node. SIMD lookup.
struct Node16 : public Node {
    uint8_t keys[16]; // aligned for SIMD in production
    Node* children[16];

    Node16() : Node(NodeType::Node16) {
        memset(keys, 0, 16);
        memset(children, 0, sizeof(children));
    }
};

// 3. Node48: Large node. Indirection array.
struct Node48 : public Node {
    uint8_t childIndex[256]; // Maps key byte -> index in 'children'
    Node* children[48];

    Node48() : Node(NodeType::Node48) {
        memset(childIndex, 255, 256); // 255 indicates empty
        memset(children, 0, sizeof(children));
    }
};

// 4. Node256: Full node. Direct mapping.
struct Node256 : public Node {
    Node* children[256];

    Node256() : Node(NodeType::Node256) {
        memset(children, 0, sizeof(children));
    }
};

// Helper to cast base Node* to specific types
template <typename T>
T* as(Node* n) { return reinterpret_cast<T*>(n); }

Node* findChild(Node* n, uint8_t keyByte) {
    switch (n->type) {
        case NodeType::Node4: {
            auto node = as<Node4>(n);
            for (int i = 0; i < node->numChildren; ++i) {
                if (node->keys[i] == keyByte) return node->children[i];
            }
            return nullptr;
        }
        case NodeType::Node16: {
            auto node = as<Node16>(n);
            // SIMD comparison
            __m128i cmp = _mm_cmpeq_epi8(_mm_set1_epi8(keyByte),
                                         _mm_loadu_si128((__m128i*)node->keys));

            // Create mask from comparison result
            int bitfield = _mm_movemask_epi8(cmp) & ((1 << node->numChildren) - 1);

            if (bitfield) {
                // Find index of first set bit
                return node->children[__builtin_ctz(bitfield)];
            }
            return nullptr;
        }
        case NodeType::Node48: {
            auto node = as<Node48>(n);
            uint8_t idx = node->childIndex[keyByte];
            if (idx != 255) return node->children[idx];
            return nullptr;
        }
        case NodeType::Node256: {
            auto node = as<Node256>(n);
            return node->children[keyByte]; // O(1) Direct Access
        }
    }
    return nullptr;
}

// Forward declarations
void addChild(Node*& node, uint8_t byte, Node* child);

// Grow logic: Node4 -> Node16
// Real impl would also handle Node16->48, 48->256
void growNode4(Node*& n) {
    auto oldNode = as<Node4>(n);
    auto newNode = new Node16();

    // Copy path compression info
    newNode->prefixLen = oldNode->prefixLen;
    memcpy(newNode->prefix, oldNode->prefix, sizeof(oldNode->prefix));

    // Copy children
    for(int i=0; i < oldNode->numChildren; ++i) {
        newNode->keys[i] = oldNode->keys[i];
        newNode->children[i] = oldNode->children[i];
    }
    newNode->numChildren = oldNode->numChildren;

    delete oldNode;
    n = newNode;
}

void addChild(Node*& n, uint8_t byte, Node* child) {
    switch (n->type) {
        case NodeType::Node4: {
            auto node = as<Node4>(n);
            if (node->numChildren >= 4) {
                growNode4(n);
                addChild(n, byte, child); // Retry with Node16
                return;
            }

            // Insert sorted (simplistic bubble insert for brevity)
            int i = 0;
            for (; i < node->numChildren; ++i) {
                if (byte < node->keys[i]) break;
            }

            // Shift
            for (int j = node->numChildren; j > i; --j) {
                node->keys[j] = node->keys[j-1];
                node->children[j] = node->children[j-1];
            }

            node->keys[i] = byte;
            node->children[i] = child;
            node->numChildren++;
            break;
        }
        case NodeType::Node16: {
            // ... similar logic (check full -> growNode16) ...
            auto node = as<Node16>(n);
            node->keys[node->numChildren] = byte;
            node->children[node->numChildren] = child;
            node->numChildren++;
            // Note: Node16 keys usually kept sorted to allow efficient range scan,
            // though strict SIMD lookup doesn't technically require it for point queries.
            break;
        }
        // ... Node48 and Node256 implementations omitted for brevity ...
    }
}

// Simplified Leaf for example (in reality, store Value or tagged pointer)
struct Leaf : public Node {
    std::string key;
    std::string value;
    Leaf(std::string k, std::string v) : Node(NodeType::Node4), key(k), value(v) {} // Hack type
};

bool isLeaf(Node* n) {
    // In production, use pointer tagging (e.g. LSB=1)
    // Here we just use a simplified check or assume external knowledge
    return false; // Placeholder
}

// Helper to check prefix mismatch
// Returns index of mismatch, or prefixLen if full match
uint32_t checkPrefix(Node* n, const std::string& key, int depth) {
    uint32_t idx = 0;
    for (; idx < n->prefixLen; ++idx) {
        if (depth + idx >= key.length() || n->prefix[idx] != key[depth + idx])
            return idx;
    }
    return idx;
}

Leaf* search(Node* root, const std::string& key) {
    Node* node = root;
    int depth = 0;

    while (node) {
        if (isLeaf(node)) {
            // Lazy expansion check
            auto leaf = reinterpret_cast<Leaf*>(node);
            if (leaf->key == key) return leaf;
            return nullptr;
        }

        // Check compressed path
        if (node->prefixLen > 0) {
            uint32_t p = checkPrefix(node, key, depth);
            if (p != node->prefixLen) return nullptr;
            depth += node->prefixLen;
        }

        node = findChild(node, key[depth]);
        depth++;
    }
    return nullptr;
}
