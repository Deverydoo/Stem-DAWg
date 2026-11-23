// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// voice-denoise: removes noise from a recording with the DeepFilterNet3 voice
// model, without the app.
//
//   voice-denoise <model folder> <input> <output.wav> [--reduction=dB]
//
// The model folder holds enc.onnx, erb_dec.onnx and df_dec.onnx. Input in any
// format the app imports; output is a 24-bit WAV at the input's sample rate.

#include <daw/ai/InferenceBackend.h>
#include <daw/ai/VoiceDenoiser.h>
#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Resampler.h>
#include <daw/media/WavWriter.h>

#include <chrono>
#include <cstdio>
#include <string>

using namespace daw;

namespace {

int fail(const std::string& message)
{
    std::fprintf(stderr, "voice-denoise: %s\n", message.c_str());
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4)
        return fail("usage: voice-denoise <model folder> <input> <output.wav> [--reduction=dB]");
    const std::filesystem::path modelDir = argv[1], input = argv[2], output = argv[3];
    VoiceDenoiseOptions options;
    for (int i = 4; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.starts_with(L"--reduction="))
            options.reductionDb = std::stod(arg.substr(12));
        else
            return fail("unknown option");
    }

    auto backend = createOnnxBackend({.intraOpThreads = 4});
    if (!backend)
        return fail(backend.error().message);
    auto denoiser = VoiceDenoiser::load(*backend, modelDir);
    if (!denoiser)
        return fail(denoiser.error().message);
    auto decoded = decodeAudioFile(input);
    if (!decoded)
        return fail(decoded.error().message);
    AudioAssetPtr audio = *decoded;
    const double rate = audio->sampleRate;
    if (rate != VoiceDenoiser::kSampleRate) {
        auto resampled = resampleAudio(*audio, VoiceDenoiser::kSampleRate);
        if (!resampled)
            return fail(resampled.error().message);
        audio = *resampled;
    }

    const auto started = std::chrono::steady_clock::now();
    auto result = AudioAsset::create(audio->numChannels(), audio->numFrames(), VoiceDenoiser::kSampleRate);
    for (int c = 0; c < audio->numChannels(); ++c) {
        auto channel = (*denoiser)->process(audio->channelData(c), static_cast<std::size_t>(audio->numFrames()), options);
        if (!channel)
            return fail(channel.error().message);
        result->channels[static_cast<std::size_t>(c)] = std::move(*channel);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    AudioAssetPtr out = result;
    if (rate != VoiceDenoiser::kSampleRate) {
        auto back = resampleAudio(*out, rate);
        if (!back)
            return fail(back.error().message);
        out = *back;
    }
    if (auto status = writeWavFile(output, *out, WavSampleFormat::Pcm24); !status)
        return fail(status.error().message);
    std::printf("%.1f s of audio, %d channel(s), in %.1f s (%.0fx real time)\n", audio->durationSeconds(), audio->numChannels(),
                seconds, audio->durationSeconds() / seconds);
    return 0;
}
