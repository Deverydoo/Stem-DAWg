// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/Destinations.h>

#include <algorithm>
#include <cmath>

namespace daw {

double destinationSampleRate(const Destination& destination, double projectRate) noexcept
{
    if (destination.sampleRate > 0.0)
        return destination.sampleRate;
    if (destination.maxSampleRate > 0.0 && projectRate > destination.maxSampleRate) {
        // 88.2 / 176.4 kHz come down to 44.1 kHz, the rest to the limit.
        const double ratio = projectRate / 44100.0;
        if (std::abs(ratio - std::round(ratio)) < 1e-9 && 44100.0 <= destination.maxSampleRate)
            return 44100.0;
        return destination.maxSampleRate;
    }
    return projectRate;
}

std::filesystem::path destinationPath(const std::filesystem::path& path, const Destination& destination)
{
    std::filesystem::path out = path;
    std::filesystem::path name = path.stem();
    name += " (";
    name += destination.key;
    name += ")";
    name += path.extension();
    out.replace_filename(name);
    return out;
}

ProjectModel masteredFor(const ProjectModel& model, const Destination& destination, const LoudnessMatch& match)
{
    ProjectModel mastered = model;
    MasterSettings master = model.master;
    master.ceilingDbtp = destination.ceilingDbtp;
    mastered.master = applyLoudnessMatch(master, match, destination.lufs);
    mastered.masterGainDb = std::clamp(match.masterGainDb, kSilenceDb, 12.0);
    return mastered;
}

Result<DestinationExportReport> exportForDestination(const ProjectModel& model, const MediaPool& pool,
                                                     const Destination& destination, const ExportRequest& request,
                                                     const RenderOptions& options, const CancellationToken& cancel)
{
    const double rate = destinationSampleRate(destination, model.sampleRate);
    // The media at the file's rate once, for the match and the render.
    auto prepared = prepareMediaAtRate(model, pool, rate, cancel);
    if (!prepared)
        return prepared.error();
    const MediaPool& renderPool = **prepared;

    // The match renders the mix and runs trials, the export renders it
    // again: about 60 / 40 of the time.
    const auto progress = [&](double from, double span) {
        return [&options, from, span](double fraction) {
            if (options.progress)
                options.progress(from + span * fraction);
        };
    };

    MasterSettings master = model.master;
    master.limiter = true;
    master.ceilingDbtp = destination.ceilingDbtp;
    LoudnessMatchOptions matchOptions;
    matchOptions.sampleRate = rate;
    matchOptions.progress = progress(0.0, 0.6);
    auto match = matchLoudness(model, renderPool, destination.lufs, master, matchOptions, cancel);
    if (!match)
        return match.error();

    ExportRequest fileRequest = request;
    fileRequest.format = destination.format;
    RenderOptions renderOptions = options;
    renderOptions.sampleRate = rate;
    renderOptions.progress = progress(0.6, 0.4);
    auto exported = exportMix(masteredFor(model, destination, *match), renderPool, fileRequest, renderOptions, cancel);
    if (!exported)
        return exported.error();
    return DestinationExportReport{*match, std::move(*exported)};
}

} // namespace daw
