// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/ProjectSerializer.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <set>

using nlohmann::json;

namespace daw {

const char* toString(MediaStorage storage) noexcept
{
    return storage == MediaStorage::Copied ? "copied" : "referenced";
}

namespace {

std::string keyToHex(std::uint64_t key)
{
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016" PRIx64, key);
    return buffer;
}

std::uint64_t hexToKey(const std::string& text)
{
    return text.empty() ? 0 : std::stoull(text, nullptr, 16);
}

MediaStorage storageFromString(const std::string& text)
{
    if (text == "copied")
        return MediaStorage::Copied;
    if (text == "referenced")
        return MediaStorage::Referenced;
    throw std::invalid_argument("unknown media storage '" + text + "'");
}

} // namespace

namespace {

json denoiseToJson(const DenoiseSettings& d)
{
    json j{{"enabled", d.enabled}, {"reductionDb", d.reductionDb}, {"sensitivityDb", d.sensitivityDb}};
    if (!d.profile.empty()) {
        // 0.01 dB steps keep the file compact (profiles are learned at that resolution).
        json power = json::array();
        for (float v : d.profile.powerDb)
            power.push_back(std::round(static_cast<double>(v) * 100.0) / 100.0);
        j["profile"] = {{"sampleRate", d.profile.sampleRate},
                        {"fftSize", d.profile.fftSize},
                        {"seconds", d.profile.seconds},
                        {"powerDb", std::move(power)}};
        if (d.profile.fromSelection)
            j["profile"]["source"] = "selection";
    }
    return j;
}

} // namespace

json cleanupToJson(const TrackCleanup& c)
{
    return {{"bypassed", c.bypassed},
            {"dcRemoval", c.dcRemoval},
            {"highPass",
             {{"enabled", c.highPass.enabled},
              {"frequencyHz", c.highPass.frequencyHz},
              {"slopeDbPerOctave", c.highPass.slopeDbPerOctave}}},
            {"expander",
             {{"enabled", c.expander.enabled},
              {"thresholdDb", c.expander.thresholdDb},
              {"ratio", c.expander.ratio},
              {"rangeDb", c.expander.rangeDb},
              {"releaseMs", c.expander.releaseMs}}},
            {"hum", {{"enabled", c.hum.enabled}, {"frequencyHz", c.hum.frequencyHz}, {"harmonics", c.hum.harmonics}}},
            {"denoise", denoiseToJson(c.denoise)},
            {"transient",
             {{"enabled", c.transient.enabled}, {"attackDb", c.transient.attackDb}, {"sustainDb", c.transient.sustainDb}}}};
}

// Missing or malformed values fall back to the defaults; numbers are clamped
// to their ranges, so a hand-edited file can never configure an unstable filter.
TrackCleanup cleanupFromJson(const json& j)
{
    TrackCleanup c;
    if (!j.is_object())
        return c;
    const auto flag = [](const json& o, const char* key, bool fallback) {
        const auto it = o.find(key);
        return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
    };
    const auto number = [](const json& o, const char* key, double fallback) {
        const auto it = o.find(key);
        return it != o.end() && it->is_number() && std::isfinite(it->get<double>()) ? it->get<double>() : fallback;
    };
    c.bypassed = flag(j, "bypassed", c.bypassed);
    c.dcRemoval = flag(j, "dcRemoval", c.dcRemoval);
    if (const auto hp = j.find("highPass"); hp != j.end() && hp->is_object()) {
        c.highPass.enabled = flag(*hp, "enabled", c.highPass.enabled);
        c.highPass.frequencyHz = number(*hp, "frequencyHz", c.highPass.frequencyHz);
        c.highPass.slopeDbPerOctave = static_cast<int>(number(*hp, "slopeDbPerOctave", c.highPass.slopeDbPerOctave));
    }
    if (const auto ex = j.find("expander"); ex != j.end() && ex->is_object()) {
        c.expander.enabled = flag(*ex, "enabled", c.expander.enabled);
        c.expander.thresholdDb = number(*ex, "thresholdDb", c.expander.thresholdDb);
        c.expander.ratio = number(*ex, "ratio", c.expander.ratio);
        c.expander.rangeDb = number(*ex, "rangeDb", c.expander.rangeDb);
        c.expander.releaseMs = number(*ex, "releaseMs", c.expander.releaseMs);
    }
    if (const auto tr = j.find("transient"); tr != j.end() && tr->is_object()) {
        c.transient.enabled = flag(*tr, "enabled", c.transient.enabled);
        c.transient.attackDb = number(*tr, "attackDb", c.transient.attackDb);
        c.transient.sustainDb = number(*tr, "sustainDb", c.transient.sustainDb);
    }
    if (const auto hum = j.find("hum"); hum != j.end() && hum->is_object()) {
        c.hum.enabled = flag(*hum, "enabled", c.hum.enabled);
        c.hum.frequencyHz = number(*hum, "frequencyHz", c.hum.frequencyHz);
        c.hum.harmonics = static_cast<int>(number(*hum, "harmonics", c.hum.harmonics));
    }
    if (const auto dn = j.find("denoise"); dn != j.end() && dn->is_object()) {
        c.denoise.enabled = flag(*dn, "enabled", c.denoise.enabled);
        c.denoise.reductionDb = number(*dn, "reductionDb", c.denoise.reductionDb);
        c.denoise.sensitivityDb = number(*dn, "sensitivityDb", c.denoise.sensitivityDb);
        if (const auto pr = dn->find("profile"); pr != dn->end() && pr->is_object()) {
            NoiseProfile& p = c.denoise.profile;
            p.sampleRate = number(*pr, "sampleRate", 0.0);
            p.fftSize = static_cast<int>(number(*pr, "fftSize", 0.0));
            p.seconds = number(*pr, "seconds", 0.0);
            if (const auto src = pr->find("source"); src != pr->end() && src->is_string())
                p.fromSelection = src->get<std::string>() == "selection";
            if (const auto pw = pr->find("powerDb"); pw != pr->end() && pw->is_array())
                for (const json& v : *pw)
                    p.powerDb.push_back(v.is_number() ? static_cast<float>(v.get<double>()) : -300.0f);
        }
    }
    c.clamp();
    return c;
}

json projectToJson(const ProjectModel& model)
{
    json doc;
    doc["format"] = ProjectModel::kFormatId;
    doc["schemaVersion"] = ProjectModel::kSchemaVersion;
    doc["app"] = {{"name", "Stem DAWg"}, {"version", model.appVersion}};
    doc["project"] = {{"id", model.id.str()},
                      {"name", model.name},
                      {"createdUtc", model.createdUtc},
                      {"modifiedUtc", model.modifiedUtc}};
    doc["timeline"] = {{"sampleRate", model.sampleRate},
                       {"tempo", {{"bpm", model.tempo.bpm}, {"numerator", model.tempo.numerator}, {"denominator", model.tempo.denominator}}}};
    doc["transport"] = {{"playheadFrames", model.playhead}};
    doc["mixer"] = {{"masterGainDb", model.masterGainDb}};
    if (!model.profileId.empty() || !model.release.empty())
        doc["release"] = {{"profileId", model.profileId}, {"fields", metadataToJson(model.release)}};

    json media = json::array();
    for (const MediaRef& m : model.media) {
        media.push_back({{"id", m.id.str()},
                         {"name", m.name},
                         {"path", m.path},
                         {"storage", toString(m.storage)},
                         {"sampleRate", m.sampleRate},
                         {"channels", m.numChannels},
                         {"frames", m.numFrames},
                         {"sourceKey", keyToHex(m.sourceKey)}});
        if (m.derivation)
            media.back()["derivedFrom"] = {{"source", m.derivation->source.str()},
                                           {"process", m.derivation->process},
                                           {"amountDb", m.derivation->amountDb}};
    }
    doc["media"] = std::move(media);

    json tracks = json::array();
    for (const Track& t : model.tracks) {
        json clips = json::array();
        for (const Clip& c : t.clips) {
            clips.push_back({{"id", c.id.str()},
                             {"name", c.name},
                             {"mediaId", c.media.str()},
                             {"timelineStart", c.timelineStart},
                             {"sourceStart", c.sourceStart},
                             {"length", c.length},
                             {"gainDb", c.gainDb}});
        }
        json track = {{"id", t.id.str()},
                      {"name", t.name},
                      {"gainDb", t.gainDb},
                      {"pan", t.pan},
                      {"muted", t.muted},
                      {"solo", t.soloed},
                      {"clips", std::move(clips)}};
        if (t.cleanup != TrackCleanup{})
            track["cleanup"] = cleanupToJson(t.cleanup);
        tracks.push_back(std::move(track));
    }
    doc["tracks"] = std::move(tracks);

    json analyses = json::array();
    for (const AnalysisRecord& a : model.analyses) {
        analyses.push_back({{"id", a.id.str()},
                            {"mediaId", a.media.str()},
                            {"analyzer", a.analyzer},
                            {"analyzerVersion", a.analyzerVersion},
                            {"result", a.resultPath},
                            {"createdUtc", a.createdUtc}});
    }
    doc["analyses"] = std::move(analyses);
    return doc;
}

Result<ProjectModel> projectFromJson(json doc, const MigrationRegistry& migrations)
{
    if (!doc.is_object())
        return makeError(ErrorCode::ParseError, "project document is not a JSON object");
    if (doc.contains("format") && doc["format"] == ProjectModel::kLegacyFormatId)
        doc["format"] = ProjectModel::kFormatId;
    if (doc.contains("format") && doc["format"] != ProjectModel::kFormatId)
        return makeError(ErrorCode::UnsupportedFormat, "not a Stem DAWg project");

    auto migrated = migrations.migrate(doc, ProjectModel::kSchemaVersion);
    if (!migrated)
        return migrated.error();
    if (!doc.contains("format") || doc["format"] != ProjectModel::kFormatId)
        return makeError(ErrorCode::UnsupportedFormat, "not a Stem DAWg project");

    ProjectModel model;
    try {
        const json& project = doc.at("project");
        model.id = ProjectId(project.at("id").get<std::string>());
        model.name = project.value("name", std::string{});
        model.createdUtc = project.value("createdUtc", std::string{});
        model.modifiedUtc = project.value("modifiedUtc", std::string{});
        model.appVersion = doc.value("app", json::object()).value("version", std::string{});

        const json& timeline = doc.at("timeline");
        model.sampleRate = timeline.at("sampleRate").get<double>();
        const json tempo = timeline.value("tempo", json::object());
        model.tempo.bpm = tempo.value("bpm", 120.0);
        model.tempo.numerator = tempo.value("numerator", 4);
        model.tempo.denominator = tempo.value("denominator", 4);

        model.playhead = doc.value("transport", json::object()).value("playheadFrames", std::int64_t{0});
        model.masterGainDb = doc.value("mixer", json::object()).value("masterGainDb", 0.0);
        const json release = doc.value("release", json::object());
        model.profileId = release.value("profileId", std::string());
        model.release = metadataFromJson(release.value("fields", json::object()));

        for (const json& m : doc.value("media", json::array())) {
            MediaRef ref;
            ref.id = MediaId(m.at("id").get<std::string>());
            ref.name = m.value("name", std::string{});
            ref.path = m.at("path").get<std::string>();
            ref.storage = storageFromString(m.value("storage", std::string("copied")));
            ref.sampleRate = m.at("sampleRate").get<double>();
            ref.numChannels = m.at("channels").get<int>();
            ref.numFrames = m.at("frames").get<std::int64_t>();
            ref.sourceKey = hexToKey(m.value("sourceKey", std::string{}));
            if (const auto d = m.find("derivedFrom"); d != m.end() && d->is_object())
                ref.derivation = MediaDerivation{MediaId(d->value("source", std::string{})), d->value("process", std::string{}),
                                                 d->value("amountDb", 0.0)};
            model.media.push_back(std::move(ref));
        }

        for (const json& t : doc.value("tracks", json::array())) {
            Track track;
            track.id = TrackId(t.at("id").get<std::string>());
            track.name = t.value("name", std::string{});
            track.gainDb = t.value("gainDb", 0.0);
            track.pan = std::clamp(t.value("pan", 0.0), -1.0, 1.0);
            track.muted = t.value("muted", false);
            track.soloed = t.value("solo", false);
            if (t.contains("cleanup"))
                track.cleanup = cleanupFromJson(t.at("cleanup"));
            for (const json& c : t.value("clips", json::array())) {
                Clip clip;
                clip.id = ClipId(c.at("id").get<std::string>());
                clip.name = c.value("name", std::string{});
                clip.media = MediaId(c.at("mediaId").get<std::string>());
                clip.timelineStart = c.at("timelineStart").get<std::int64_t>();
                clip.sourceStart = c.value("sourceStart", std::int64_t{0});
                clip.length = c.at("length").get<std::int64_t>();
                clip.gainDb = c.value("gainDb", 0.0);
                track.clips.push_back(std::move(clip));
            }
            model.tracks.push_back(std::move(track));
        }

        for (const json& a : doc.value("analyses", json::array())) {
            AnalysisRecord record;
            record.id = AnalysisId(a.at("id").get<std::string>());
            record.media = MediaId(a.at("mediaId").get<std::string>());
            record.analyzer = a.at("analyzer").get<std::string>();
            record.analyzerVersion = a.value("analyzerVersion", 0);
            record.resultPath = a.value("result", std::string{});
            record.createdUtc = a.value("createdUtc", std::string{});
            model.analyses.push_back(std::move(record));
        }
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("invalid project document: ") + e.what());
    }

    if (Status valid = validateProject(model); !valid)
        return valid.error();
    return model;
}

std::string serializeProject(const ProjectModel& model)
{
    return projectToJson(model).dump(2) + "\n";
}

Result<ProjectModel> parseProject(std::string_view text, const MigrationRegistry& migrations)
{
    json doc = json::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded())
        return makeError(ErrorCode::ParseError, "project.json is not valid JSON");
    return projectFromJson(std::move(doc), migrations);
}

Status validateProject(const ProjectModel& model)
{
    auto fail = [](std::string message) { return Status(makeError(ErrorCode::Corrupt, std::move(message))); };

    if (!model.id.valid())
        return fail("project has no id");
    if (!(model.sampleRate > 0.0))
        return fail("project sample rate must be positive");
    if (!(model.tempo.bpm > 0.0) || model.tempo.numerator <= 0 || model.tempo.denominator <= 0)
        return fail("invalid tempo or time signature");
    if (model.playhead < 0)
        return fail("negative playhead");

    std::set<std::string> ids;
    auto unique = [&](const std::string& id) { return !id.empty() && ids.insert(id).second; };

    for (const MediaRef& m : model.media) {
        if (!unique(m.id.str()))
            return fail("missing or duplicate media id '" + m.id.str() + "'");
        if (m.path.empty() || !(m.sampleRate > 0.0) || m.numChannels <= 0 || m.numFrames < 0)
            return fail("invalid media entry '" + m.id.str() + "'");
    }
    for (const MediaRef& m : model.media)
        if (m.derivation && (m.derivation->source == m.id || model.findMedia(m.derivation->source) == nullptr))
            return fail("media '" + m.id.str() + "' is derived from unknown media '" + m.derivation->source.str() + "'");
    for (const Track& t : model.tracks) {
        if (!unique(t.id.str()))
            return fail("missing or duplicate track id '" + t.id.str() + "'");
        if (!std::isfinite(t.gainDb) || !(t.pan >= -1.0 && t.pan <= 1.0))
            return fail("track '" + t.id.str() + "' has an invalid gain or pan");
        for (const Clip& c : t.clips) {
            if (!unique(c.id.str()))
                return fail("missing or duplicate clip id '" + c.id.str() + "'");
            if (model.findMedia(c.media) == nullptr)
                return fail("clip '" + c.id.str() + "' references unknown media '" + c.media.str() + "'");
            if (c.timelineStart < 0 || c.sourceStart < 0 || c.length < 0)
                return fail("clip '" + c.id.str() + "' has a negative position or length");
        }
    }
    for (const AnalysisRecord& a : model.analyses) {
        if (!unique(a.id.str()))
            return fail("missing or duplicate analysis id '" + a.id.str() + "'");
        if (model.findMedia(a.media) == nullptr)
            return fail("analysis '" + a.id.str() + "' references unknown media");
    }
    return Status::success();
}

} // namespace daw
