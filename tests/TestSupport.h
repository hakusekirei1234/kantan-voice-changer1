// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

// 依存を増やさないための最小テストフレームワーク。
// GoogleTest を入れると CI のクローン量とビルド時間が跳ねるうえ、
// ここで要るのは「登録・実行・比較・要約」の 4 つだけなので自前で持つ。
//
// コンソール出力は ASCII のみ。CI のログも Windows のコードページ 932 も通す。

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace kvctest
{

//==============================================================================
struct TestCase
{
    const char* suite;
    const char* name;
    void (*run)();
};

std::vector<TestCase>& registry();

struct Registrar
{
    Registrar (const char* suite, const char* name, void (*fn)())
    {
        registry().push_back ({ suite, name, fn });
    }
};

/** 個々のチェック失敗で投げる。テストは 1 件目の失敗で打ち切る
    （DSP のテストは失敗が連鎖して出力が読めなくなるため）。 */
struct Failure
{
    std::string message;
};

void reportCheck (bool ok, const std::string& message);

#define KVC_TEST(suite, name)                                                        \
    static void suite##_##name();                                                     \
    static ::kvctest::Registrar kvcReg_##suite##_##name (#suite, #name, suite##_##name); \
    static void suite##_##name()

#define KVC_CHECK(cond)                                                              \
    ::kvctest::reportCheck ((cond), std::string ("CHECK failed: ") + #cond            \
                                    + "  [" + __FILE__ + ":" + std::to_string (__LINE__) + "]")

/** 相対誤差での比較。DSP の測定値は絶対誤差では意味を持たないので既定はこちら。 */
void checkNear (double actual, double expected, double relTolerance,
                const char* what, const char* file, int line);

#define KVC_CHECK_NEAR(actual, expected, relTol) \
    ::kvctest::checkNear ((actual), (expected), (relTol), #actual, __FILE__, __LINE__)

void checkInRange (double actual, double low, double high,
                   const char* what, const char* file, int line);

#define KVC_CHECK_IN_RANGE(actual, low, high) \
    ::kvctest::checkInRange ((actual), (low), (high), #actual, __FILE__, __LINE__)

/** 失敗したときだけログに残る補足情報。成功時は何も出さない。 */
void note (const std::string& text);

//==============================================================================
// 信号生成・測定のヘルパ。
//==============================================================================

struct Formant
{
    double frequency;
    double bandwidth;
};

/** 声門パルス列 + 共振器 3 段の合成母音。
    実マイク音ではないが、F0 と F1..F3 が既知なので
    「ピッチだけ動いたか」「フォルマントだけ動いたか」を数値で分離できる。

    @param jitterPercent  周期ゆらぎ。0 だと完全周期になり、実声より簡単すぎる。 */
std::vector<float> makeVowel (double sampleRate, double f0, double seconds,
                              const std::vector<Formant>& formants,
                              double jitterPercent = 0.5);

/** ホワイトノイズ。決定的（固定シードの xorshift）なので CI で揺れない。 */
std::vector<float> makeNoise (int numSamples, float amplitude, unsigned int seed = 1u);

/** CMNDF(YIN) による F0 推定。区間の中央付近を複数窓で測って中央値を返す。
    @return Hz。有声と判定できなければ 0。 */
double estimateF0 (const float* data, int numSamples, double sampleRate,
                   double minHz = 50.0, double maxHz = 800.0);

/** 振幅スペクトルの重心 (Hz)。フォルマント移動の粗い指標。 */
double spectralCentroid (const float* data, int numSamples, double sampleRate,
                         double lowHz = 100.0, double highHz = 5000.0);

/** ケプストラム・リフタリングで求めたスペクトル包絡のピーク周波数 (Hz)。
    倍音の櫛を均して F1 付近を拾うので、F0 を動かしても値が動かない。 */
double envelopePeakHz (const float* data, int numSamples, double sampleRate,
                       double lowHz = 200.0, double highHz = 3000.0);

/** 2 つの信号の「均した対数スペクトル」の距離 (dB rms)。
    平均レベルの差は正規化して落とすので、比べるのは形だけ。
    ピッチ変換の品質を「理想の合成音」との距離として数値化するために使う。 */
double logSpectralDistanceDb (const float* a, int numSamplesA,
                              const float* b, int numSamplesB,
                              double sampleRate, double lowHz = 200.0, double highHz = 5000.0);

/** 振幅の揺れの深さ。OLA に隙間が空くとここが跳ねる（ブツブツ感の指標）。
    5 ms フレームの rms を取り、中央値に対する標準偏差の比を返す。 */
double amplitudeModulationDepth (const float* data, int numSamples, double sampleRate);

double rms (const float* data, int numSamples);
double peak (const float* data, int numSamples);
bool   allFinite (const float* data, int numSamples);

} // namespace kvctest
