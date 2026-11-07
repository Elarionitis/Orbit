#pragma once

#include "common/metrics.h"

namespace orbit::server {

// Every metric the server publishes. Resolved once at startup, so the hot path
// holds direct references and never goes near the registry.
struct ServerMetrics {
    explicit ServerMetrics(metrics::Registry& r)
        : connections_active(r.gauge("orbit_connections_active", "Open client connections.")),
          connections_accepted(
              r.counter("orbit_connections_accepted_total", "Connections accepted.")),
          connections_rejected(
              r.counter("orbit_connections_rejected_total", "Connections refused at the limit.")),
          sessions_active(r.gauge("orbit_sessions_active", "Authenticated sessions.")),
          rooms_active(r.gauge("orbit_rooms", "Rooms on this server.")),
          messages_published(
              r.counter("orbit_messages_published_total", "Messages committed to a room.")),
          messages_delivered(
              r.counter("orbit_messages_delivered_total", "Messages handed to recipients.")),
          direct_messages(r.counter("orbit_direct_messages_total", "Direct messages relayed.")),
          rate_limited(
              r.counter("orbit_rate_limited_total", "Messages refused by the rate limiter.")),
          overloaded(r.counter("orbit_overload_rejections_total", "Messages shed under load.")),
          protocol_errors(
              r.counter("orbit_protocol_errors_total", "Connections dropped for bad frames.")),
          closed_peer(r.counter("orbit_disconnects_total", "Connections closed, by cause.",
                                "reason=\"peer\"")),
          closed_error(r.counter("orbit_disconnects_total", "Connections closed, by cause.",
                                 "reason=\"io_error\"")),
          closed_local(r.counter("orbit_disconnects_total", "Connections closed, by cause.",
                                 "reason=\"local\"")),
          closed_slow(r.counter("orbit_disconnects_total", "Connections closed, by cause.",
                                "reason=\"slow_consumer\"")),
          closed_tls(r.counter("orbit_disconnects_total", "Connections closed, by cause.",
                               "reason=\"tls\"")),
          publish_latency(r.histogram("orbit_publish_latency_seconds",
                                      "Time from reading a message to committing it to its room.",
                                      {0.00001, 0.00005, 0.0001, 0.00025, 0.0005, 0.001, 0.0025,
                                       0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 1.0})) {}

    metrics::Gauge& connections_active;
    metrics::Counter& connections_accepted;
    metrics::Counter& connections_rejected;
    metrics::Gauge& sessions_active;
    metrics::Gauge& rooms_active;
    metrics::Counter& messages_published;
    metrics::Counter& messages_delivered;
    metrics::Counter& direct_messages;
    metrics::Counter& rate_limited;
    metrics::Counter& overloaded;
    metrics::Counter& protocol_errors;
    metrics::Counter& closed_peer;
    metrics::Counter& closed_error;
    metrics::Counter& closed_local;
    metrics::Counter& closed_slow;
    metrics::Counter& closed_tls;
    metrics::Histogram& publish_latency;
};

}  // namespace orbit::server
