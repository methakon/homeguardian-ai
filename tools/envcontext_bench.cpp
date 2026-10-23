// Standalone benchmark for the environmental context engine (HG-EDGE-05).
//
// Deliberately NOT part of the test target: this measures cost, not correctness, and
// it is compiled on demand so the test suite stays a pure functional gate.
//
//   g++ -std=c++20 -O2 -I src -I <nlohmann include> -o /tmp/bench
//       tools/envcontext_bench.cpp src/core/EnvironmentalContext.cpp
//
// Reports ingest throughput, snapshot cost and RSS, over the channel set the Mi A3
// can actually produce (illuminance + motion + device thermal; no atmospheric).

#include "core/EnvironmentalContext.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace homeguardian;
using namespace std::chrono;

namespace {

long rss_kb() {
    std::ifstream f("/proc/self/statm");
    long size = 0, resident = 0;
    f >> size >> resident;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

EnvironmentalReading make(const std::string& channel, double value,
                          system_clock::time_point at, double sigma, double q) {
    EnvironmentalReading r;
    r.channel = channel;
    r.source = "node:mi-a3-04de";
    r.value = value;
    r.observed_at = at;
    r.received_at = at;
    r.provenance = Provenance::synthetic;
    r.quality = q;
    r.uncertainty = sigma;
    r.uncertainty_declared = sigma > 0.0;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const int ingests = argc > 1 ? std::atoi(argv[1]) : 200000;
    const int snapshots = argc > 2 ? std::atoi(argv[2]) : 20000;

    // Exactly the channels the Mi A3 can supply.
    const std::vector<std::string> channels = {
        "illuminance",
        "motion.presence",
        "motion.acceleration_magnitude",
        "device.battery.temperature",
        "device.soc.temperature",
    };

    auto now = system_clock::now();
    EnvironmentalContextEngine engine;

    const long rss_before = rss_kb();
    auto t0 = steady_clock::now();
    for (int i = 0; i < ingests; ++i) {
        const std::string& ch = channels[static_cast<size_t>(i) % channels.size()];
        double value = 0.0;
        if (ch == "illuminance") value = 50.0 + (i % 400);
        else if (ch == "motion.presence") value = (i % 2);
        else if (ch == "motion.acceleration_magnitude") value = 9.81 + (i % 20) * 0.01;
        else value = 30.0 + (i % 150) * 0.1;
        // Timestamps spread over the last minute so freshness logic engages.
        engine.ingest(make(ch, value, now - milliseconds(i % 60000), 2.0, 0.9), now);
    }
    auto t1 = steady_clock::now();
    const long rss_after = rss_kb();

    std::size_t sink = 0;
    auto t2 = steady_clock::now();
    for (int i = 0; i < snapshots; ++i) {
        ContextSnapshot snap = engine.snapshot(now);
        sink += snap.channels.size();
        if (snap.atmospheric_series_available()) sink += 1000;  // never taken here
    }
    auto t3 = steady_clock::now();

    const double ingest_us = duration_cast<nanoseconds>(t1 - t0).count() / 1000.0 / ingests;
    const double snap_us = duration_cast<nanoseconds>(t3 - t2).count() / 1000.0 / snapshots;

    std::printf("environmental context engine benchmark\n");
    std::printf("  channels simulated     : %zu (the Mi A3's available set)\n", channels.size());
    std::printf("  ingest                 : %d readings in %.1f ms  = %.2f us/reading  (%.0f readings/s)\n",
                ingests, duration_cast<microseconds>(t1 - t0).count() / 1000.0,
                ingest_us, 1e6 / ingest_us);
    std::printf("  snapshot               : %d snapshots in %.1f ms  = %.2f us/snapshot\n",
                snapshots, duration_cast<microseconds>(t3 - t2).count() / 1000.0, snap_us);
    std::printf("  engine RSS (bounded)   : %ld kB -> %ld kB (delta %+ld kB)\n",
                rss_before, rss_after, rss_after - rss_before);
    std::printf("  retained readings      : %zu (bounded by max_readings_per_channel)\n",
                engine.reading_count());

    ContextSnapshot snap = engine.snapshot(now);
    std::printf("  atmospheric available  : %s\n", snap.atmospheric_series_available() ? "true" : "false");
    std::printf("  battery temperature    : %s\n",
                snap.battery_temperature() ? std::to_string(*snap.battery_temperature()).c_str() : "null");
    std::printf("  checksum               : %zu\n", sink);
    return 0;
}
