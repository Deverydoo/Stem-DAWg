// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// stem-report: prints the Stem DAWg stem report for audio files or a folder
// of stems, without the desktop app. Useful for batch checks and for tuning
// the advice rules.
//
//   stem-report <folder | file...> [--json] [--apply-fixes] [--denoise[=dB]]
//               [--bleed[=light|medium|strong]] [--master[=LUFS]] [--target=folder]
//               [--out=folder]
//
// --apply-fixes applies every clean-up fix the advice suggests; --denoise
// learns each stem's noise profile, looks for hum, and switches on noise
// reduction (default 12 dB) and hum removal where found; --bleed renders
// bleed-suppressed copies of every stem (default medium), into
// <out>/Media/Processed when --out is given (to listen to), otherwise into a
// temporary folder removed afterwards. --master sets the master fader for a
// loudness target (default -14 LUFS) through the master limiter at -1 dBTP.
// --target builds a tonal-balance target from the audio files in a folder
// (reference tracks), compares the mix with it and suggests master EQ;
// with --apply-fixes the EQ is applied.
// With any of them, the stems are reported again, measured after the
// processing.

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/session/BleedTools.h>
#include <daw/session/MasteringTools.h>
#include <daw/session/NoiseTools.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/StemReport.h>
#include <daw/session/VoiceTools.h>

#include <chrono>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <cstdlib>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace daw;

namespace {

bool isAudio(const fs::path& path)
{
    std::string ext = pathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto supported = supportedAudioExtensions();
    return std::find(supported.begin(), supported.end(), ext) != supported.end();
}

std::string db(double value)
{
    if (value <= kLoudnessFloor + 1.0)
        return "-inf";
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", value);
    return text;
}

void print(const StemReport& r)
{
    std::printf("Mix: %s LUFS integrated, LRA %.1f LU, true peak %s dBTP, %lld overs  (%.1f s analysed in %.2f s)\n\n",
                db(r.mix.integratedLufs).c_str(), r.mix.loudnessRangeLu, db(r.mix.truePeakDbtp).c_str(),
                static_cast<long long>(r.mixOvers), static_cast<double>(r.frames) / r.sampleRate, r.analysisSeconds);
    // Resonances: the strongest peak's frequency and mean cut, e.g. "3.7k/2.3".
    const auto resonance = [](const ResonanceFinding& f) {
        char text[32] = "-";
        if (f.hz > 0.0)
            std::snprintf(text, sizeof text, "%.1fk/%.1f", f.hz / 1000.0, f.cutDb);
        return std::string(text);
    };
    // Stereo: side below 120 Hz relative to mid, and the range that loses most
    // in a mono sum, e.g. "-3.0>-1.2" (as it is > with phase alignment).
    const auto lowSide = [](const TrackStemReport& t) {
        char text[32] = "-";
        if (t.lowSideDb)
            std::snprintf(text, sizeof text, "%.0f/%.0f%%", *t.lowSideDb, t.lowSideShare * 100.0);
        return std::string(text);
    };
    const auto mono = [](const TrackStemReport& t) {
        const auto worst = std::min_element(t.mono.begin(), t.mono.end(),
                                            [](const MonoBand& a, const MonoBand& b) { return a.lossDb < b.lossDb; });
        char text[32];
        std::snprintf(text, sizeof text, "%.1f>%.1f", worst->lossDb, worst->alignedLossDb);
        return std::string(text);
    };
    std::printf("%-22s %6s %7s %7s %6s %7s %6s %6s %6s %7s %6s %7s %10s %10s %9s %10s  %s\n", "Track", "Share", "LUFS", "Peak",
                "Active", "Floor", "Low", "Sub", "Corr", "Centre", "Hits/m", "Attack", "Harsh", "Shimmer", "LowSide", "Mono",
                "Clean-up");
    for (const auto& t : r.tracks)
        std::printf("%-22.22s %5.1f%% %7s %7s %5.0f%% %7s %5.0f%% %5.0f%% %6s %6.0fHz %6.0f %5sms %10s %10s %9s %10s  %s\n", t.name.c_str(),
                    t.levelShare * 100.0,
                    db(t.loudness.integratedLufs).c_str(), db(t.loudness.samplePeakDbfs).c_str(), t.activeFraction * 100.0,
                    t.noiseFloorDbfs ? db(*t.noiseFloorDbfs).c_str() : "-", t.lowEndShare * 100.0,
                    t.bandShare[0] * 100.0, t.stereoCorrelation ? std::to_string(*t.stereoCorrelation).substr(0, 5).c_str() : "-",
                    t.centroidHz, t.hitsPerMinute, t.attackMs ? db(*t.attackMs).c_str() : "-", resonance(t.harshness).c_str(),
                    resonance(t.shimmer).c_str(), lowSide(t).c_str(), mono(t).c_str(), describeCleanup(t.cleanup).c_str());
    std::printf("\nStrongest overlaps (low / mid / high / full):\n");
    auto pairs = r.pairs;
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.correlation[3] > b.correlation[3]; });
    for (std::size_t i = 0; i < std::min<std::size_t>(pairs.size(), 10); ++i)
        std::printf("  %-18.18s + %-18.18s %5.2f %5.2f %5.2f %5.2f\n", r.tracks[pairs[i].a].name.c_str(),
                    r.tracks[pairs[i].b].name.c_str(), pairs[i].correlation[0], pairs[i].correlation[1],
                    pairs[i].correlation[2], pairs[i].correlation[3]);
    std::printf("\nAdvice:\n");
    for (const auto& a : r.advice) {
        std::printf("  [%s] %s\n", toString(a.severity), a.message.c_str());
        for (const auto& fix : a.fixes)
            std::printf("      fix: %s -> %s\n", r.tracks[fix.track].name.c_str(), describeCleanup(fix.applyTo({})).c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    bool json = false;
    bool applyFixes = false;
    std::optional<double> denoiseDb;
    const BleedStrength* bleed = nullptr;
    std::optional<double> masterLufs;
    std::optional<fs::path> targetDir;
    std::optional<fs::path> out;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json = true;
            continue;
        }
        if (arg == "--apply-fixes") {
            applyFixes = true;
            continue;
        }
        if (arg == "--denoise" || arg.rfind("--denoise=", 0) == 0) {
            denoiseDb = arg.size() > 10 ? std::atof(arg.c_str() + 10) : 12.0;
            continue;
        }
        if (arg == "--bleed" || arg.rfind("--bleed=", 0) == 0) {
            const std::string name = arg.size() > 8 ? arg.substr(8) : "medium";
            for (const BleedStrength& s : kBleedStrengths) {
                std::string lower = s.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == name)
                    bleed = &s;
            }
            if (bleed == nullptr) {
                std::fprintf(stderr, "--bleed takes light, medium or strong\n");
                return 2;
            }
            continue;
        }
        if (arg.rfind("--target=", 0) == 0) {
            targetDir = utf8ToPath(arg.substr(9));
            continue;
        }
        if (arg == "--master" || arg.rfind("--master=", 0) == 0) {
            masterLufs = arg.size() > 9 ? std::atof(arg.c_str() + 9) : -14.0;
            continue;
        }
        if (arg.rfind("--out=", 0) == 0) {
            out = utf8ToPath(arg.substr(6));
            continue;
        }
        const fs::path path = utf8ToPath(arg);
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            for (const auto& entry : fs::directory_iterator(path, ec))
                if (entry.is_regular_file() && isAudio(entry.path()))
                    files.push_back(entry.path());
        } else {
            files.push_back(path);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: stem-report <folder | audio files...> [--json] [--apply-fixes] [--denoise[=dB]] [--bleed[=light|medium|strong]] [--master[=LUFS]] [--target=folder] [--out=folder]\n");
        return 2;
    }
    std::sort(files.begin(), files.end());

    ProjectModel model = makeEmptyProject("stem-report");
    MediaPool pool;
    std::vector<std::string> stems;
    for (const fs::path& file : files)
        stems.push_back(pathToUtf8(file.stem()));
    const std::vector<std::string> names = shortTrackNames(stems);
    for (std::size_t f = 0; f < files.size(); ++f) {
        const fs::path& file = files[f];
        auto audio = decodeAudioFile(file);
        if (!audio) {
            std::fprintf(stderr, "%s: %s\n", pathToUtf8(file).c_str(), audio.error().describe().c_str());
            return 1;
        }
        if (model.media.empty())
            model.sampleRate = (*audio)->sampleRate;
        MediaRef ref{MediaId::generate(), pathToUtf8(file.stem()), pathToUtf8(file.filename()), MediaStorage::Referenced,
                     (*audio)->sampleRate, (*audio)->numChannels(), (*audio)->numFrames(), 1};
        model.media.push_back(ref);
        addMediaAsNewTrack(model, ref, 0);
        model.tracks.back().name = names[f];
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = *audio;
        pool.put(std::move(loaded));
    }

    auto report = analyzeStems(model, pool);
    if (!report) {
        std::fprintf(stderr, "analysis failed: %s\n", report.error().describe().c_str());
        return 1;
    }
    if (targetDir) {
        std::vector<fs::path> references;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(*targetDir, ec))
            if (entry.is_regular_file() && isAudio(entry.path()))
                references.push_back(entry.path());
        std::sort(references.begin(), references.end());
        std::vector<std::string> skipped;
        const auto target = tonalTargetFromFiles(pathToUtf8(targetDir->filename()), references, &skipped);
        const auto mixProfile = tonalProfileOfMix(model, pool);
        if (!target || !mixProfile) {
            std::fprintf(stderr, "tonal comparison failed: %s\n",
                         (!target ? target.error() : mixProfile.error()).describe().c_str());
            return 1;
        }
        std::array<float, kMasterEqBandCount> current{};
        for (std::size_t b = 0; b < current.size(); ++b)
            current[b] = model.master.eq ? static_cast<float>(model.master.eqGainDb[b]) : 0.0f;
        const TonalComparison cmp = compareTonal(*mixProfile, *target, current);
        if (!json) {
            std::printf("Tonal balance against %zu reference(s)%s:\n", target->references.size(),
                        skipped.empty() ? "" : " (some unreadable)");
            std::printf("%8s %8s %7s %7s %7s\n", "Band", "Target", "Spread", "Mix", "Diff");
            for (std::size_t b = 0; b < kTonalBands; ++b)
                std::printf("%7.0fHz %7.1f %7.1f %7.1f %+7.1f%s\n", kTonalBandHz[b], target->meanDb[b], target->spreadDb[b],
                            mixProfile->levelDb[b], cmp.deviationDb[b], cmp.outside[b] ? "  *" : "");
            std::printf("Suggested master EQ (dB): low %+.1f  low-mid %+.1f  mid %+.1f  presence %+.1f  air %+.1f"
                        "  (deviation %.1f -> %.1f dB)\n\n",
                        cmp.eqGainDb[0], cmp.eqGainDb[1], cmp.eqGainDb[2], cmp.eqGainDb[3], cmp.eqGainDb[4], cmp.rmsBeforeDb,
                        cmp.rmsAfterDb);
        }
        // The mix again with only the suggested EQ changed, to check the prediction.
        ProjectModel withEq = model;
        withEq.master.eq = true;
        for (std::size_t b = 0; b < kMasterEqBandCount; ++b)
            withEq.master.eqGainDb[b] = cmp.eqGainDb[b];
        withEq.master.tonalTarget = toSettings(*target);
        if (!json)
            if (const auto after = tonalProfileOfMix(withEq, pool)) {
                const TonalComparison again = compareTonal(*after, *target, cmp.eqGainDb);
                std::printf("Measured with the suggested EQ: deviation %.1f dB; per band:", again.rmsBeforeDb);
                for (std::size_t b = 0; b < kTonalBands; ++b)
                    std::printf(" %+.1f", again.deviationDb[b]);
                std::printf("\nCompared again it would suggest: %+.1f %+.1f %+.1f %+.1f %+.1f\n\n", again.eqGainDb[0],
                            again.eqGainDb[1], again.eqGainDb[2], again.eqGainDb[3], again.eqGainDb[4]);
            }
        if (applyFixes)
            model.master = withEq.master;
    }
    if (applyFixes || denoiseDb || bleed || masterLufs) {
        if (!json) {
            std::printf("Before clean-up:\n\n");
            print(*report);
            std::printf("\n");
        }
        std::size_t changed = 0;
        if (bleed) {
            std::error_code ec;
            const fs::path root = out ? *out : fs::temp_directory_path(ec) / ("stem-report-bleed-" + std::to_string(std::rand()));
            const auto started = std::chrono::steady_clock::now();
            auto result = suppressBleed(
                root, model, [&](const MediaId& id) { auto m = pool.get(id); return m ? m->original : AudioAssetPtr{}; },
                bleedTracks(model), *bleed, 0.0);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (!result) {
                std::fprintf(stderr, "bleed suppression failed: %s\n", result.error().describe().c_str());
                return 1;
            }
            if (!json)
                std::printf("Bleed suppression (%s) in %.1f s:\n", bleed->name, seconds);
            for (BleedTrackOutcome& outcome : *result) {
                const Track* track = model.findTrack(outcome.track);
                if (!json)
                    std::printf("  %-22.22s music %+.2f dB, quietest tenth %+.1f dB\n", track->name.c_str(), outcome.musicChangeDb,
                                outcome.bleedChangeDb);
                applyProcessedMedia(model, outcome.track, outcome.media);
                for (LoadedMedia& loaded : outcome.loaded)
                    pool.put(std::move(loaded));
                ++changed;
            }
            if (!json && out)
                std::printf("  copies written to %s\n", pathToUtf8(root / "Media" / "Processed").c_str());
            if (!out)
                fs::remove_all(root, ec);
            if (!json)
                std::printf("\n");
        }
        if (applyFixes)
            changed += applyCleanupFixes(model, *report, allCleanupFixes(*report));
        if (denoiseDb) {
            for (Track& track : model.tracks) {
                const auto hum = detectHum(model, pool, track.id);
                if (hum && hum->has_value()) {
                    track.cleanup.hum = {true, (*hum)->frequencyHz, (*hum)->harmonics};
                    if (!json)
                        std::printf("%-22.22s hum at %.0f Hz, %d harmonics, %.0f dB above the noise\n", track.name.c_str(),
                                    (*hum)->frequencyHz, (*hum)->harmonics, (*hum)->prominenceDb);
                }
                auto profile = learnNoiseProfile(model, pool, track.id);
                if (!profile) {
                    if (!json)
                        std::printf("%-22.22s no noise profile: %s\n", track.name.c_str(), profile.error().message.c_str());
                    continue;
                }
                if (!json)
                    std::printf("%-22.22s noise learned from %.1f s\n", track.name.c_str(), profile->seconds);
                track.cleanup.denoise.enabled = true;
                track.cleanup.denoise.reductionDb = *denoiseDb;
                track.cleanup.denoise.profile = std::move(*profile);
                ++changed;
            }
        }
        if (masterLufs) {
            MasterSettings master = model.master;
            master.limiter = true;
            const auto match = matchLoudness(model, pool, *masterLufs, master);
            if (!match) {
                std::fprintf(stderr, "loudness match failed: %s\n", match.error().describe().c_str());
                return 1;
            }
            if (!json)
                std::printf("Master for %.1f LUFS: mix %.1f LUFS / %+.1f dBTP -> fader %+.1f dB -> %.1f LUFS, %+.1f dBTP, "
                            "LRA %.1f LU, limiter down to %.1f dB (%d trials, %.1f s)%s\n",
                            *masterLufs, match->mixLufs, match->mixTruePeakDbtp, match->masterGainDb, match->resultLufs,
                            match->resultTruePeakDbtp, match->resultLoudnessRangeLu, match->maxReductionDb, match->trials,
                            match->seconds,
                            match->reached ? "" : match->limited ? "  (target needs more limiting than allowed)" : "  (target not reached)");
            model.masterGainDb = match->masterGainDb;
            model.master = master;
            model.master.targetLufs = *masterLufs;
            ++changed;
        }
        auto cleaned = analyzeStems(model, pool);
        if (!cleaned) {
            std::fprintf(stderr, "analysis failed: %s\n", cleaned.error().describe().c_str());
            return 1;
        }
        if (json) {
            std::printf("%s\n", toJson(*cleaned).dump(2).c_str());
            return 0;
        }
        std::printf("\n\nAfter clean-up (%zu change(s)):\n\n", changed);
        print(*cleaned);
        return 0;
    }
    if (json) {
        std::printf("%s\n", toJson(*report).dump(2).c_str());
        return 0;
    }
    print(*report);
    return 0;
}
