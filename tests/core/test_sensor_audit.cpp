// test_sensor_audit: the questions asked of the two Linux-side components after they were built.
//
// An audit is not a re-run of the happy path. These tests drive RECONNECT, REVOCATION, REPLAY ACROSS a
// reconnect, and a source that changes its provenance claim mid-stream - the states a long-running
// appliance actually reaches - and they pin what the components do in each one.

#include <catch2/catch.hpp>

#include "backend/linux/AlsaCommandAudioSource.h"
#include "core/ActivityPattern.h"
#include "core/AnimalActivity.h"
#include "core/SensorEventConsumer.h"

#include <chrono>
#include <string>
#include <vector>

using namespace homeguardian;

namespace {

auto now() { return std::chrono::system_clock::time_point(std::chrono::seconds(1'800'000'000)); }

double epoch(std::chrono::system_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

nlohmann::json event_json(const std::string& id, const std::string& provenance = "real",
                          const std::string& consent = "granted", const std::string& source = "device:/dev/video0") {
    return nlohmann::json{{"type", "inference"},
                          {"schema_version", 1},
                          {"observation_id", id},
                          {"source", source},
                          {"provenance", provenance},
                          {"status", "detected"},
                          {"detector_id", "camera-motion-activity"},
                          {"observed_at", epoch(now())},
                          {"confidence", 0.8},
                          {"consent", consent},
                          {"features", {{"motion_level", 0.2}}}};
}

SensorEventConsumer make_consumer() {
    SensorEventConsumer::Config cfg;
    cfg.trusted_source_prefixes = {"device:", "android:"};
    return SensorEventConsumer(cfg);
}

}  // namespace

TEST_CASE("audit: consent revocation under reconnect stops and restarts capture", "[audit][consent]") {
    auto c = make_consumer();
    REQUIRE(c.ingest(event_json("obs-1"), now()).accepted);

    // The operator revokes consent: nothing more enters the window.
    REQUIRE(c.ingest(event_json("obs-2", "real", "revoked"), now()).code == IngestCode::consent_missing);
    REQUIRE_FALSE(c.source_available());
    REQUIRE(c.window().size() == 1);

    // THE RECONNECT CASE: after a revocation, an observation that simply OMITS the consent field must be
    // refused too. A producer that drops the field is not thereby consenting.
    auto without_field = event_json("obs-3");
    without_field.erase("consent");
    REQUIRE(c.ingest(without_field, now()).code == IngestCode::consent_missing);
    REQUIRE(c.window().size() == 1);

    // Consent granted again: capture resumes, and the source is available again.
    REQUIRE(c.ingest(event_json("obs-4"), now()).accepted);
    REQUIRE(c.source_available());
    REQUIRE(c.window().size() == 2);
}

TEST_CASE("audit: replay protection survives a reconnect", "[audit][replay]") {
    auto c = make_consumer();
    REQUIRE(c.ingest(event_json("obs-1"), now()).accepted);

    // a disconnect
    REQUIRE(c.ingest(event_json("obs-2", "real", "granted", "device:/dev/video0"), now()).accepted);
    auto offline = event_json("obs-3");
    offline["status"] = "unavailable";
    REQUIRE(c.ingest(offline, now()).code == IngestCode::source_offline);

    // and a reconnect that REPLAYS the first observation: refused, so a replayed stream cannot manufacture
    // a run of observations where none occurred.
    REQUIRE(c.ingest(event_json("obs-1"), now()).code == IngestCode::duplicate);
    REQUIRE(c.window().size() == 2);
    REQUIRE(c.accepted() == 2);
}

TEST_CASE("audit: a source that changes its provenance claim is judged per observation",
          "[audit][provenance]") {
    auto c = make_consumer();
    REQUIRE(c.ingest(event_json("obs-1", "real"), now()).accepted);
    REQUIRE(c.ingest(event_json("obs-2", "synthetic"), now()).accepted);

    // The consumer honours each claim and does NOT retro-label the earlier one.
    REQUIRE(c.window()[0].provenance == Provenance::real);
    REQUIRE(c.window()[1].provenance == Provenance::synthetic);

    // And the fused verdict reports the mixture as mixed, rather than as whichever member was convenient.
    const auto fused = c.fuse();
    REQUIRE(fused.contributing == 2);
    REQUIRE(fused.provenance == EvidenceProvenance::mixed);
    REQUIRE(fused.real_inputs == 1);
    REQUIRE(fused.synthetic_inputs == 1);
}

TEST_CASE("audit: the window is bounded, so a long reconnect cannot grow it", "[audit][bounds]") {
    SensorEventConsumer::Config cfg;
    cfg.trusted_source_prefixes = {"device:"};
    cfg.max_window = 4;
    auto c = SensorEventConsumer(cfg);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(c.ingest(event_json("obs-" + std::to_string(i)), now()).accepted);
    }
    REQUIRE(c.window().size() == 4);
    REQUIRE(c.accepted() == 20);          // every observation was accepted
    REQUIRE(c.window().front().observation_id == "obs-16");   // and the oldest were dropped, not lost
}

TEST_CASE("audit: a source that reappears is usable again through the same consumer", "[audit][reconnect]") {
    auto c = make_consumer();
    auto offline = event_json("obs-1");
    offline["status"] = "unavailable";
    REQUIRE(c.ingest(offline, now()).code == IngestCode::source_offline);
    REQUIRE_FALSE(c.source_available());
    REQUIRE(c.window().empty());                       // no evidence was invented from the gap

    auto later = event_json("obs-2");
    later["observed_at"] = epoch(now() + std::chrono::seconds(1));
    REQUIRE(c.ingest(later, now() + std::chrono::seconds(1)).accepted);
    REQUIRE(c.source_available());
    REQUIRE(c.window().size() == 1);
}

TEST_CASE("audit: the microphone adapter recovers after the device disappears", "[audit][audio]") {
    // A capture that fails is contained to its window; the next window can succeed, with no restart.
    std::vector<CommandResult> scripted{
        CommandResult{0, "**** List of CAPTURE Hardware Devices ****\ncard 0: PCH\n", "", false},  // -l
        CommandResult{1, "", "device busy", false},                                                // window 1
        CommandResult{0, std::string(24000 * 2, '\x01'), "", false},                               // window 2
    };
    size_t call = 0;
    AlsaCommandAudioSource::Config cfg;
    cfg.runner = [&scripted, &call](const std::vector<std::string>&, size_t) {
        return call < scripted.size() ? scripted[call++] : CommandResult{1, "", "exhausted", false};
    };
    cfg.seconds = 1;
    AlsaCommandAudioSource source(cfg);
    std::string error;
    REQUIRE(source.open(&error));

    std::vector<int16_t> pcm;
    size_t frames = 0;
    uint32_t channels = 0, rate = 0;
    REQUIRE_FALSE(source.read_window(pcm, &frames, &channels, &rate));      // busy: no window
    REQUIRE(source.read_window(pcm, &frames, &channels, &rate));            // recovered
    REQUIRE(frames == 24000);
    REQUIRE(source.last_read_was_short());
}

TEST_CASE("audit: the animal layer and the pattern analysis stay separate vocabularies",
          "[audit][separation]") {
    // Research-only pattern labels must not be readable as species identification. The guard is that the
    // two vocabularies are disjoint and that neither pattern name nor its uncertainty is a species claim.
    const ActivityPattern patterns[] = {ActivityPattern::quiet, ActivityPattern::isolated_change,
                                        ActivityPattern::repeated_localized,
                                        ActivityPattern::sustained_activity, ActivityPattern::erratic};
    const AnimalActivityClass animals[] = {AnimalActivityClass::motion_only,
                                           AnimalActivityClass::candidate_animal_activity};
    for (const auto p : patterns) {
        const std::string pattern_name = activity_pattern_to_string(p);
        for (const auto a : animals) {
            INFO(pattern_name << " vs " << animal_activity_class_to_string(a));
            REQUIRE(pattern_name != animal_activity_class_to_string(a));
        }
    }
    // And a pattern finding never carries a species: its uncertainty says so, and it has no field that
    // could hold one.
    ActivityPatternConfig cfg;
    cfg.enabled = true;
    const std::vector<ActivitySample> samples(5, ActivitySample{0.010, 0.08, 0.05});
    const auto finding = characterise_activity_pattern(samples, cfg);
    REQUIRE(finding.uncertainty.find("does not identify a species") != std::string::npos);
    REQUIRE(finding.description.find("species") == std::string::npos);
}
