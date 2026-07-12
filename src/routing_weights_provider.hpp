// Ranvier Core - Routing-Score Weights Provider Interface
//
// Optional, pluggable source of route-scorer weights. By default the scorer's
// per-signal weights come only from static config (routing.scoring.*). This
// seam lets an operator supply those weights at runtime from an external source
// — a centrally-managed policy or configuration service — without the router
// hard-depending on any concrete provider. The stock build installs the no-op
// (NoopRoutingWeightsProvider), so behaviour is identical to config-only, bit-
// for-bit.
//
// Mirrors the telemetry-sink seam (src/telemetry_sink.hpp): one abstract entry
// point, a no-op default, and a process-wide factory slot chosen at start-up.
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - weights_for() is consulted OFF the request path only — from the periodic
//     refresh that materialises a shard-local snapshot (see RouterService's
//     routing-weights refresh). The per-request scorer NEVER calls it; it reads
//     the already-published snapshot. Therefore weights_for() itself must be a
//     cheap, non-blocking lookup into whatever the provider already holds
//     locally: any fetch from the external policy source happens on the
//     provider's own schedule/thread, never inside this call. (Same discipline
//     as a telemetry sink offloading its export — the reactor is never blocked.)
//
//   - Returning std::nullopt means "no opinion" for that key: the scorer then
//     uses the configured routing.scoring.* weights unchanged. A provider that
//     has no opinion for any key leaves routing exactly as config-only.
//
//   - The lookup key is resolvable by the scorer at scoring time from the anchor
//     backend's operator labels (see RoutingWeightsKey). weights_for() must
//     treat unknown keys as nullopt rather than inventing weights.

#pragma once

#include "route_scorer.hpp"  // ScoringWeights (no new weight type is introduced)
#include "types.hpp"         // BackendType, HardwareLabel

#include <absl/container/flat_hash_map.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ranvier {

// =============================================================================
// RoutingWeightsKey
// =============================================================================
//
// The routing context a provider is asked about. These are exactly the anchor
// backend's operator-set labels, which the scorer already has in hand at
// scoring time (one shard-local BackendInfo lookup it performs anyway), so the
// per-request resolve costs nothing beyond a hash-map probe.
//
// Deliberately a routing-local struct rather than reusing TelemetryBucketKey:
// the scorer path (get_backend_for_prefix) does not carry the request's
// workload/intent, so a workload dimension could not be satisfied per request.
// These three dimensions are all content-free and operator-bounded (model
// family / engine class / hardware label), so provider cardinality stays small.
struct RoutingWeightsKey {
    std::string   model_family;                            // "" allowed (unlabeled fleet)
    BackendType   backend_type   = BackendType::VLLM;      // engine class
    HardwareLabel hardware_label = HardwareLabel::UNSPECIFIED;

    bool operator==(const RoutingWeightsKey& other) const {
        return backend_type   == other.backend_type
            && hardware_label == other.hardware_label
            && model_family   == other.model_family;
    }
};

// Hash for absl::flat_hash_map. Standard boost-style mixer over the dimensions;
// no correctness dependency on the specific mix — purely distributional (same
// shape as TelemetryBucketKeyHash).
struct RoutingWeightsKeyHash {
    size_t operator()(const RoutingWeightsKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.model_family);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.backend_type))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.hardware_label))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// Published, shard-local override map: key -> weights. Empty in the stock build
// (no provider) — the scorer then reads config.scoring unchanged. A key absent
// from the map means "use the configured baseline for that key".
using RoutingWeightsSnapshot =
    absl::flat_hash_map<RoutingWeightsKey, ScoringWeights, RoutingWeightsKeyHash>;

// =============================================================================
// RoutingWeightsProvider
// =============================================================================

// Abstract provider. See the contract above.
class RoutingWeightsProvider {
public:
    virtual ~RoutingWeightsProvider() = default;

    // Advisory. Off-request-path only. Returns the weights this provider wants
    // applied for `key`, or std::nullopt for "no opinion" (scorer keeps the
    // configured routing.scoring.*). Must not block the reactor (contract).
    virtual std::optional<ScoringWeights> weights_for(const RoutingWeightsKey& key) = 0;
};

// The default: always "no opinion". Installed when no provider is configured,
// which leaves the scorer on its configured-weights path unchanged.
class NoopRoutingWeightsProvider final : public RoutingWeightsProvider {
public:
    std::optional<ScoringWeights> weights_for(const RoutingWeightsKey& /*key*/) override {
        return std::nullopt;
    }
};

inline std::unique_ptr<RoutingWeightsProvider> make_default_routing_weights_provider() {
    return std::make_unique<NoopRoutingWeightsProvider>();
}

// =============================================================================
// Off-request-path snapshot build + hot-path resolve (both pure, reactor-free)
// =============================================================================

// Consult `provider` once per distinct key to build the map that gets published
// to the shards. Called from the periodic refresh, never on the request path.
// Keys the provider has no opinion on are omitted, so the scorer falls through
// to the configured baseline for them. O(keys); `keys` is bounded by the live
// backend count (Rule #4/#17).
inline RoutingWeightsSnapshot build_routing_weights_snapshot(
    RoutingWeightsProvider& provider,
    const std::vector<RoutingWeightsKey>& keys) {
    RoutingWeightsSnapshot snapshot;
    snapshot.reserve(keys.size());
    for (const auto& key : keys) {
        if (auto weights = provider.weights_for(key)) {
            snapshot.insert_or_assign(key, *weights);
        }
    }
    return snapshot;
}

// Hot-path resolve (shard-local, O(1)): the weights the scorer should use for
// `key`. Returns the published override when present, else `baseline`
// UNCHANGED. This is a pointer/read, never a per-request recomputation: when
// the snapshot is empty (stock build, or provider had no opinion anywhere) it
// is a single empty-check and the baseline reference is returned as-is — the
// safe-default guarantee.
inline const ScoringWeights& resolve_scoring_weights(
    const RoutingWeightsSnapshot& snapshot,
    const RoutingWeightsKey& key,
    const ScoringWeights& baseline) {
    if (snapshot.empty()) {
        return baseline;
    }
    auto it = snapshot.find(key);
    return it != snapshot.end() ? it->second : baseline;
}

// =============================================================================
// Factory slot (process-wide seam, mirrors set_telemetry_sink_factory)
// =============================================================================
//
// The stock build leaves this unset and falls back to
// make_default_routing_weights_provider() (the no-op). A downstream build
// installs a real provider via set_routing_weights_provider_factory() during
// its own start-up, BEFORE the router arms the routing-weights refresh. An empty
// factory resets to the default.
//
// Not racy runtime state (cf. Hard Rule #11): the slot is written once and read
// once, both on shard 0 during the single-threaded start-up sequence, and never
// touched on the hot path or from other shards. The function-local static
// supplies the standard thread-safe one-time init of the slot itself.
using RoutingWeightsProviderFactory =
    std::function<std::unique_ptr<RoutingWeightsProvider>()>;

inline RoutingWeightsProviderFactory& routing_weights_provider_factory_slot() {
    static RoutingWeightsProviderFactory slot;  // empty until a build installs one
    return slot;
}

inline void set_routing_weights_provider_factory(RoutingWeightsProviderFactory factory) {
    routing_weights_provider_factory_slot() = std::move(factory);
}

// The installed factory, or make_default_routing_weights_provider (the no-op)
// when unset.
inline RoutingWeightsProviderFactory get_routing_weights_provider_factory() {
    const RoutingWeightsProviderFactory& slot = routing_weights_provider_factory_slot();
    return slot ? slot
                : RoutingWeightsProviderFactory{&make_default_routing_weights_provider};
}

}  // namespace ranvier
