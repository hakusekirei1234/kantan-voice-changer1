// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace kvc
{

//==============================================================================
// エンジン全体の固定量。
//==============================================================================

/** エンジン内部は常に 48 kHz を要求する。デバイスが拒否した場合だけ
    実際に開けたレートで動かし、DriftRing の nominalRatio が差を吸収する。 */
inline constexpr double kPreferredSampleRate = 48000.0;

/** PSOLA の先読み。設定値・バイパス・ピッチ量に関わらず常にこの値。
    可変にすると送信/モニターの遅延補正が切替のたびにずれてプチッと鳴る。 */
inline constexpr int kPsolaLookaheadSamples = 512;

/** ノイズ除去の STFT サイズ（= そのままアルゴリズム遅延サンプル数）。 */
inline constexpr int kNoiseFftLowLatency = 256;   // HOP 64,  5.33 ms（既定）
inline constexpr int kNoiseFftHighQuality = 512;  // HOP 128, 10.67 ms

inline constexpr int kNoiseHopLowLatency = 64;
inline constexpr int kNoiseHopHighQuality = 128;

/** 内部処理は最後までモノラル。各シンクで最後にファンアウトする。 */
inline constexpr int kInternalNumChannels = 1;

/** 全デバイスの最大ブロック長。prepare 時の確保サイズの上限。 */
inline constexpr int kMaxBlockSize = 4096;

//==============================================================================

enum class NoiseQuality
{
    lowLatency256 = 0,   ///< N=256 / HOP=64
    highQuality512 = 1   ///< N=512 / HOP=128
};

inline int fftSizeForQuality (NoiseQuality q) noexcept
{
    return q == NoiseQuality::highQuality512 ? kNoiseFftHighQuality : kNoiseFftLowLatency;
}

inline int hopSizeForQuality (NoiseQuality q) noexcept
{
    return q == NoiseQuality::highQuality512 ? kNoiseHopHighQuality : kNoiseHopLowLatency;
}

/** DSP チェーン全体のアルゴリズム遅延。デバイスバッファは含まない。 */
inline int dspLatencySamples (NoiseQuality q) noexcept
{
    return kPsolaLookaheadSamples + fftSizeForQuality (q);   // 既定: 512 + 256 = 768
}

//==============================================================================

enum class NoiseStrength
{
    weak = 0,     ///< 弱  最大減衰 6 dB
    normal = 1,   ///< 標準 最大減衰 12 dB（既定）
    strong = 2    ///< 強  最大減衰 18 dB
};

enum class PresetKind
{
    original = 0,   ///< 元の声
    higher,         ///< 高め（女性寄り）
    lower,          ///< 低め（男性寄り）
    cute            ///< かわいい
};

struct PresetValues
{
    float pitchSemitones;
    float formantSemitones;
    bool  pitchEnabled;
    bool  formantEnabled;
};

/** ui.json のプリセット定義。MainComponent のチップ行がそのまま使う。 */
inline PresetValues presetValues (PresetKind k) noexcept
{
    switch (k)
    {
        case PresetKind::higher: return {  4.0f,  3.0f, true,  true  };
        case PresetKind::lower:  return { -4.0f, -3.0f, true,  true  };
        case PresetKind::cute:   return {  7.0f,  5.0f, true,  true  };
        case PresetKind::original:
        default:                 return {  0.0f,  0.0f, false, false };
    }
}

//==============================================================================
/** オーディオスレッドがブロック先頭で 1 回だけ作る、平たい値のコピー。
    以降そのブロックの中では atomic を二度と読まない（ブロック途中で値が
    変わると SmoothedValue のターゲットが跳ねて段差になるため）。 */
struct VoiceParamSnapshot
{
    float pitchSemitones = 0.0f;      ///< pitchEnabled が false のとき既に 0 に潰してある
    float formantSemitones = 0.0f;    ///< formantEnabled が false のとき既に 0 に潰してある
    bool  shifterActive = false;      ///< pitchEnabled || formantEnabled
    bool  noiseSuppressionEnabled = false;
    NoiseStrength noiseStrength = NoiseStrength::normal;
    bool  muted = false;
    bool  muteAlsoSilencesMonitor = true;
    float inputGainLinear = 1.0f;
    float outputGainLinear = 1.0f;
};

//==============================================================================
/** GUI が書き、オーディオスレッドが読むパラメータ一式。

    スレッド契約:
      - 各 setter は「どのスレッドからでも」安全。
      - snapshot() は「オーディオスレッド専用」だが、確保もロックもしないので
        メッセージスレッドから呼んでも実害はない。
      - noiseQuality だけは別扱い。これは VoiceEngine::prepare() でしか読まれず、
        変更するとアルゴリズム遅延が変わる。GUI から変えたら必ず
        AudioEngine::restartAsync() を呼んでグラフを作り直すこと。
        ランタイム中に遅延が変わってはならない（要件 4）。
*/
struct VoiceParams
{
    std::atomic<float> pitchSemitones   { 0.0f };   // -12 .. +12
    std::atomic<float> formantSemitones { 0.0f };   // -12 .. +12
    std::atomic<bool>  pitchEnabled     { false };
    std::atomic<bool>  formantEnabled   { false };

    std::atomic<bool>  noiseSuppressionEnabled { false };
    std::atomic<int>   noiseStrength { static_cast<int> (NoiseStrength::normal) };

    /** prepare 時のみ有効。実行中の変更はエンジン再構築が必要。 */
    std::atomic<int>   noiseQuality { static_cast<int> (NoiseQuality::lowLatency256) };

    std::atomic<bool>  muted { false };
    /** 詳細設定「ミュート中は自分の声も止める」。既定 ON。
        送信バスの無音化はこの値に関わらず常に行われる。
        ビープはこの無音化より後段で足すので、ON でも切替音は鳴る。 */
    std::atomic<bool>  muteAlsoSilencesMonitor { true };

    std::atomic<float> inputGainDb  { 0.0f };
    std::atomic<float> outputGainDb { 0.0f };

    /** ミュート切替時のビープ音量（0..1）。0 で無効と同義。 */
    std::atomic<float> beepVolume { 0.6f };
    std::atomic<bool>  beepEnabled { true };

    //==========================================================================
    static float dbToLinear (float db) noexcept
    {
        return db <= -100.0f ? 0.0f : std::pow (10.0f, db * 0.05f);
    }

    /** どのスレッドからでも呼べる。確保・ロック・例外なし。 */
    VoiceParamSnapshot snapshot() const noexcept
    {
        VoiceParamSnapshot s;

        const bool pOn = pitchEnabled.load   (std::memory_order_relaxed);
        const bool fOn = formantEnabled.load (std::memory_order_relaxed);

        s.pitchSemitones   = pOn ? pitchSemitones.load   (std::memory_order_relaxed) : 0.0f;
        s.formantSemitones = fOn ? formantSemitones.load (std::memory_order_relaxed) : 0.0f;
        s.shifterActive    = pOn || fOn;

        s.noiseSuppressionEnabled = noiseSuppressionEnabled.load (std::memory_order_relaxed);
        s.noiseStrength = static_cast<NoiseStrength> (noiseStrength.load (std::memory_order_relaxed));

        s.muted = muted.load (std::memory_order_relaxed);
        s.muteAlsoSilencesMonitor = muteAlsoSilencesMonitor.load (std::memory_order_relaxed);

        s.inputGainLinear  = dbToLinear (inputGainDb.load  (std::memory_order_relaxed));
        s.outputGainLinear = dbToLinear (outputGainDb.load (std::memory_order_relaxed));

        return s;
    }

    NoiseQuality getNoiseQuality() const noexcept
    {
        return static_cast<NoiseQuality> (noiseQuality.load (std::memory_order_relaxed));
    }

    /** プリセットチップ用。メッセージスレッドから呼ぶ。 */
    void applyPreset (PresetKind k) noexcept
    {
        const auto v = presetValues (k);
        pitchSemitones.store   (v.pitchSemitones,   std::memory_order_relaxed);
        formantSemitones.store (v.formantSemitones, std::memory_order_relaxed);
        pitchEnabled.store     (v.pitchEnabled,     std::memory_order_relaxed);
        formantEnabled.store   (v.formantEnabled,   std::memory_order_relaxed);
    }

    /** ホットキーハンドラ（メッセージスレッド）から。戻り値は新しいミュート状態。 */
    bool toggleMute() noexcept
    {
        const bool m = ! muted.load (std::memory_order_relaxed);
        muted.store (m, std::memory_order_relaxed);
        return m;
    }
};

} // namespace kvc
