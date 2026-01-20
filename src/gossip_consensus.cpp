// Ranvier Core - Gossip Consensus Module Implementation
//
// Manages cluster quorum and peer liveness tracking.

#include "gossip_consensus.hpp"

#include <cmath>
#include <sstream>

#include <boost/range/irange.hpp>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>

namespace ranvier {

GossipConsensus::GossipConsensus(const ClusterConfig& config)
    : _config(config) {
}

seastar::future<> GossipConsensus::start(const std::vector<seastar::socket_address>& initial_peers) {
    _running = true;

    // Only shard 0 manages the peer table
    if (seastar::this_shard_id() != 0) {
        co_return;
    }

    // Initialize peer table with all initial peers
    auto now = seastar::lowres_clock::now();
    for (const auto& peer : initial_peers) {
        _peer_table[peer] = {now, true, std::nullopt};
    }

    // Initialize alive count (all peers start alive)
    _stats_cluster_peers_alive = _peer_table.size();

    // Calculate initial quorum state
    if (_config.quorum_enabled) {
        size_t total_nodes = _peer_table.size() + 1;  // +1 for self
        size_t required = quorum_required();
        size_t alive_nodes = _stats_cluster_peers_alive + 1;  // +1 for self

        if (alive_nodes >= required) {
            _quorum_state = QuorumState::HEALTHY;
            _stats_quorum_state = 1;
            log_gossip_consensus().info("Quorum initialized: HEALTHY (alive={}, required={}, total={})",
                                        alive_nodes, required, total_nodes);
        } else {
            _quorum_state = QuorumState::DEGRADED;
            _stats_quorum_state = 0;
            log_gossip_consensus().warn("Quorum initialized: DEGRADED - insufficient peers "
                                        "(alive={}, required={}, total={})",
                                        alive_nodes, required, total_nodes);
        }
    }

    // Set up liveness check timer with RAII timer safety
    _liveness_timer.set_callback([this] {
        // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
        // The holder must outlive the work, so declare outside try block.
        seastar::gate::holder timer_holder;
        try {
            timer_holder = _timer_gate.hold();
        } catch (const seastar::gate_closed_exception&) {
            return;
        }
        check_liveness();
    });
    _liveness_timer.arm_periodic(_config.gossip_heartbeat_interval);

    log_gossip_consensus().info("Consensus module started with {} peers", _peer_table.size());
    co_return;
}

seastar::future<> GossipConsensus::stop() {
    if (!_running) {
        co_return;
    }

    log_gossip_consensus().info("Stopping consensus module");
    _running = false;

    // Close timer gate first (waits for in-flight callbacks)
    co_await _timer_gate.close();

    // Cancel timers
    _liveness_timer.cancel();

    co_return;
}

void GossipConsensus::update_peer_seen(const seastar::socket_address& peer) {
    auto it = _peer_table.find(peer);
    if (it != _peer_table.end()) {
        it->second.last_seen = seastar::lowres_clock::now();
        if (!it->second.is_alive) {
            log_gossip_consensus().info("Peer {} recovered", peer);
            it->second.is_alive = true;
        }
    }
}

void GossipConsensus::associate_backend(const seastar::socket_address& peer, BackendId id) {
    auto it = _peer_table.find(peer);
    if (it != _peer_table.end()) {
        it->second.associated_backend = id;
    }
}

void GossipConsensus::add_peer(const seastar::socket_address& peer) {
    if (_peer_table.find(peer) == _peer_table.end()) {
        _peer_table[peer] = {seastar::lowres_clock::now(), true, std::nullopt};
        log_gossip_consensus().info("Peer added: {}", peer);
    }
}

void GossipConsensus::remove_peer(const seastar::socket_address& peer) {
    auto it = _peer_table.find(peer);
    if (it != _peer_table.end()) {
        // Prune routes for removed peer if it had an associated backend
        // NOTE: std::function is passed across shards here. This relies on Small Buffer
        // Optimization (SBO) to avoid cross-shard heap issues. The callback lambda only
        // captures a pointer, which fits in SBO. A proper fix would make callbacks
        // shard-aware (each shard registers its own callback).
        // TODO: Refactor to use per-shard callback registration for full memory safety.
        if (it->second.associated_backend && _route_prune_callback) {
            BackendId b_id = *it->second.associated_backend;
            auto callback = _route_prune_callback;
            (void)seastar::parallel_for_each(
                boost::irange<unsigned>(0, seastar::smp::count),
                [callback, b_id](unsigned shard_id) {
                    return seastar::smp::submit_to(shard_id, [callback, b_id] {
                        return callback(b_id);
                    });
                }).handle_exception([b_id](auto ep) {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& e) {
                        log_gossip_consensus().error("Route prune callback failed for backend {}: {}", b_id, e.what());
                    }
                });
        }
        _peer_table.erase(it);
        log_gossip_consensus().info("Peer removed: {}", peer);
    }
}

std::vector<seastar::socket_address> GossipConsensus::update_peer_list(
    const std::vector<seastar::socket_address>& new_peers) {

    std::vector<seastar::socket_address> newly_added;
    auto now = seastar::lowres_clock::now();

    // Build set of new peers for lookup
    std::unordered_map<seastar::socket_address, PeerState> new_peer_table;

    for (const auto& peer : new_peers) {
        auto it = _peer_table.find(peer);
        if (it != _peer_table.end()) {
            // Preserve existing state
            new_peer_table[peer] = it->second;
        } else {
            // New peer
            new_peer_table[peer] = {now, true, std::nullopt};
            newly_added.push_back(peer);
            log_gossip_consensus().info("DNS discovery: new peer added: {}", peer);
        }
    }

    // Log and prune removed peers
    for (const auto& [peer, state] : _peer_table) {
        if (new_peer_table.find(peer) == new_peer_table.end()) {
            log_gossip_consensus().info("DNS discovery: peer removed: {}", peer);

            // Prune routes for removed peers if they had an associated backend
            // NOTE: See comment in remove_peer() about std::function SBO reliance.
            if (state.associated_backend && _route_prune_callback) {
                BackendId b_id = *state.associated_backend;
                auto callback = _route_prune_callback;
                (void)seastar::parallel_for_each(
                    boost::irange<unsigned>(0, seastar::smp::count),
                    [callback, b_id](unsigned shard_id) {
                        return seastar::smp::submit_to(shard_id, [callback, b_id] {
                            return callback(b_id);
                        });
                    }).handle_exception([b_id](auto ep) {
                        try { std::rethrow_exception(ep); }
                        catch (const std::exception& e) {
                            log_gossip_consensus().error("Route prune callback failed for backend {}: {}", b_id, e.what());
                        }
                    });
            }
        }
    }

    // Replace peer table
    _peer_table = std::move(new_peer_table);

    // Update alive count
    uint64_t alive_count = 0;
    for (const auto& [addr, state] : _peer_table) {
        if (state.is_alive) {
            ++alive_count;
        }
    }
    _stats_cluster_peers_alive = alive_count;

    return newly_added;
}

size_t GossipConsensus::quorum_required() const {
    // Quorum = floor(N * threshold) + 1 where N is total peers (including self)
    // For standard majority (threshold=0.5), this gives N/2+1
    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t required = static_cast<size_t>(std::floor(total_nodes * _config.quorum_threshold)) + 1;

    // Cap at total_nodes to prevent impossible quorum requirements
    return std::min(required, total_nodes);
}

void GossipConsensus::check_liveness() {
    auto now = seastar::lowres_clock::now();
    uint64_t alive_count = 0;

    for (auto& [addr, state] : _peer_table) {
        if (state.is_alive && (now - state.last_seen) > _config.gossip_peer_timeout) {
            state.is_alive = false;
            log_gossip_consensus().warn("Peer marked dead: socket_address={}", addr);

            // Prune routes for dead peer if it had an associated backend
            // NOTE: See comment in remove_peer() about std::function SBO reliance.
            if (state.associated_backend && _route_prune_callback) {
                BackendId b_id = *state.associated_backend;
                auto callback = _route_prune_callback;

                // Broadcast the prune command to ALL shards
                (void)seastar::parallel_for_each(boost::irange<unsigned>(0, seastar::smp::count),
                    [callback, b_id](unsigned shard_id) {
                        return seastar::smp::submit_to(shard_id, [callback, b_id] {
                            return callback(b_id);
                        });
                    }).handle_exception([b_id](auto ep) {
                        try { std::rethrow_exception(ep); }
                        catch (const std::exception& e) {
                            log_gossip_consensus().error("Route prune callback failed for backend {}: {}", b_id, e.what());
                        }
                    });
            }
        }

        if (state.is_alive) {
            ++alive_count;
        }
    }

    _stats_cluster_peers_alive = alive_count;

    // Update quorum state after liveness check
    if (_config.quorum_enabled) {
        check_quorum();
    }
}

void GossipConsensus::check_quorum() {
    // Only run on shard 0 since it manages the peer table
    if (seastar::this_shard_id() != 0) {
        return;
    }

    auto now = seastar::lowres_clock::now();
    size_t recently_seen = 0;

    // Count peers seen within the quorum check window
    for (const auto& [addr, state] : _peer_table) {
        auto time_since_seen = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_seen);
        if (time_since_seen <= _config.quorum_check_window) {
            ++recently_seen;
        }
    }

    _stats_peers_recently_seen = recently_seen;

    // Use recently seen count for quorum calculation (more strict than just alive)
    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t recently_seen_nodes = recently_seen + 1;  // +1 for self (we're always "seen")
    size_t required = quorum_required();

    QuorumState new_state = (recently_seen_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

    // Handle state transitions
    if (new_state != _quorum_state) {
        ++_quorum_transitions;

        if (new_state == QuorumState::DEGRADED) {
            _quorum_warning_active = false;
            log_gossip_consensus().error("QUORUM LOST (check_quorum): Cluster entering DEGRADED mode. "
                                         "Only {}/{} nodes recently seen within {}s window (need {} for quorum). "
                                         "New route propagation will be rejected to prevent split-brain divergence.",
                                         recently_seen_nodes, total_nodes, _config.quorum_check_window.count(), required);
        } else {
            log_gossip_consensus().info("QUORUM RESTORED (check_quorum): Cluster returning to HEALTHY mode. "
                                        "{}/{} nodes recently seen within {}s window (need {} for quorum). "
                                        "Route propagation re-enabled.",
                                        recently_seen_nodes, total_nodes, _config.quorum_check_window.count(), required);
        }

        _quorum_state = new_state;
        _stats_quorum_state = (new_state == QuorumState::HEALTHY) ? 1 : 0;
    }

    // Check for warning threshold
    if (_config.quorum_warning_threshold > 0 && new_state == QuorumState::HEALTHY) {
        size_t margin = recently_seen_nodes - required;
        bool should_warn = margin <= _config.quorum_warning_threshold;

        if (should_warn && !_quorum_warning_active) {
            _quorum_warning_active = true;
            log_gossip_consensus().warn("QUORUM WARNING: Only {} node(s) above quorum threshold "
                                        "(recently_seen={}, required={}, total={}). Cluster at risk of split-brain.",
                                        margin, recently_seen_nodes, required, total_nodes);
        } else if (!should_warn && _quorum_warning_active) {
            _quorum_warning_active = false;
            log_gossip_consensus().info("QUORUM WARNING CLEARED: Cluster has sufficient margin "
                                        "(recently_seen={}, required={}, total={}).",
                                        recently_seen_nodes, required, total_nodes);
        }
    }
}

void GossipConsensus::update_quorum_state() {
    // Only run on shard 0 since it manages the peer table
    if (seastar::this_shard_id() != 0) {
        return;
    }

    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t alive_nodes = _stats_cluster_peers_alive + 1;  // +1 for self
    size_t required = quorum_required();

    QuorumState new_state = (alive_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

    if (new_state != _quorum_state) {
        ++_quorum_transitions;

        if (new_state == QuorumState::DEGRADED) {
            _quorum_warning_active = false;
            log_gossip_consensus().error("QUORUM LOST: Cluster entering DEGRADED mode. "
                                         "Only {}/{} nodes reachable (need {} for quorum). "
                                         "New route writes will be rejected to prevent split-brain divergence.",
                                         alive_nodes, total_nodes, required);
        } else {
            log_gossip_consensus().info("QUORUM RESTORED: Cluster returning to HEALTHY mode. "
                                        "{}/{} nodes reachable (need {} for quorum). "
                                        "Route writes re-enabled.",
                                        alive_nodes, total_nodes, required);
        }

        _quorum_state = new_state;
        _stats_quorum_state = (new_state == QuorumState::HEALTHY) ? 1 : 0;
    }

    // Check for warning threshold
    if (_config.quorum_warning_threshold > 0 && new_state == QuorumState::HEALTHY) {
        size_t margin = alive_nodes - required;
        bool should_warn = margin <= _config.quorum_warning_threshold;

        if (should_warn && !_quorum_warning_active) {
            _quorum_warning_active = true;
            log_gossip_consensus().warn("QUORUM WARNING: Only {} node(s) above quorum threshold "
                                        "(alive={}, required={}, total={}). Cluster at risk of split-brain.",
                                        margin, alive_nodes, required, total_nodes);
        } else if (!should_warn && _quorum_warning_active) {
            _quorum_warning_active = false;
            log_gossip_consensus().info("QUORUM WARNING CLEARED: Cluster has sufficient margin "
                                        "(alive={}, required={}, total={}).",
                                        alive_nodes, required, total_nodes);
        }
    }
}

void GossipConsensus::start_resync() {
    log_gossip_consensus().info("Starting gossip re-sync mode - rejecting new gossip tasks");
    _resyncing.store(true, std::memory_order_relaxed);

    // SECURITY: DO NOT clear sequence windows here - they must persist
    // to prevent replay attacks. See is_duplicate() in gossip_protocol.cpp
}

void GossipConsensus::end_resync() {
    log_gossip_consensus().info("Ending gossip re-sync mode - resuming normal gossip operations");
    _resyncing.store(false, std::memory_order_relaxed);
}

ClusterState GossipConsensus::get_cluster_state() const {
    ClusterState state;

    state.quorum_state = (_quorum_state == QuorumState::HEALTHY) ? "HEALTHY" : "DEGRADED";
    state.quorum_required = quorum_required();
    state.peers_alive = _stats_cluster_peers_alive;
    state.total_peers = _peer_table.size();
    state.peers_recently_seen = _stats_peers_recently_seen;
    state.is_draining = _draining.load(std::memory_order_relaxed);
    state.local_backend_id = _local_backend_id;

    // Collect peer information
    for (const auto& [addr, peer_state] : _peer_table) {
        PeerInfo info;

        // Extract address and port from socket_address
        std::ostringstream oss;
        oss << addr;
        std::string addr_str = oss.str();

        // Parse address:port format
        if (!addr_str.empty() && addr_str.back() >= '0' && addr_str.back() <= '9') {
            auto colon_pos = addr_str.find_last_of(':');
            if (colon_pos != std::string::npos && colon_pos > 0) {
                bool is_port_separator = (addr_str[colon_pos - 1] == ']') ||
                                         (addr_str.find('[') == std::string::npos);
                if (is_port_separator) {
                    info.address = addr_str.substr(0, colon_pos);
                    try {
                        info.port = static_cast<uint16_t>(std::stoi(addr_str.substr(colon_pos + 1)));
                    } catch (const std::exception&) {
                        info.port = 0;
                    }
                } else {
                    info.address = addr_str;
                    info.port = 0;
                }
            } else {
                info.address = addr_str;
                info.port = 0;
            }
        } else {
            info.address = addr_str;
            info.port = 0;
        }

        info.is_alive = peer_state.is_alive;
        auto now = seastar::lowres_clock::now();
        info.last_seen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - peer_state.last_seen).count();
        info.associated_backend = peer_state.associated_backend;

        state.peers.push_back(std::move(info));
    }

    return state;
}

}  // namespace ranvier
