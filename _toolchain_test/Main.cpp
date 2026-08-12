// Pitchproof が「申告する遅延」と「実際の遅延」を突き合わせる。
// Cubase では体感ゼロなのに簡単ボイチェンでは遅れる、という報告の原因切り分け。

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "audio/Vst2Host.h"

#include <cmath>
#include <vector>

namespace
{
    juce::String reportText;
    void line (const juce::String& s) { reportText << s << juce::newLine; }

    constexpr double kRate = 48000.0;

    /** 実際の遅延をインパルス応答で測る。
        戻り値は「入力した位置から何サンプル後に出力が立ったか」。 */
    int measureImpulseDelay (kvc::LoadedPlugin& plugin, int blockSize, float threshold)
    {
        const int preRoll = 20;                      // 内部状態を落ち着かせる
        const int tail = 200;                        // 応答を拾う長さ
        std::vector<float> buf ((size_t) blockSize, 0.0f);

        // 無音を流して安定させる
        for (int b = 0; b < preRoll; ++b)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            plugin.processMono (buf.data(), blockSize);
        }

        // インパルスを 1 個入れる
        std::fill (buf.begin(), buf.end(), 0.0f);
        buf[0] = 1.0f;
        plugin.processMono (buf.data(), blockSize);

        for (int i = 0; i < blockSize; ++i)
            if (std::abs (buf[(size_t) i]) > threshold)
                return i;

        // 同じブロックで出なければ後続ブロックを追う
        for (int b = 1; b < tail; ++b)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            plugin.processMono (buf.data(), blockSize);

            for (int i = 0; i < blockSize; ++i)
                if (std::abs (buf[(size_t) i]) > threshold)
                    return b * blockSize + i;
        }

        return -1;
    }

    /** 正弦バーストで相互相関を取り、群遅延を測る。インパルスが通らない
        プラグイン（内部で窓を掛けるもの）向けの裏取り。 */
    int measureCorrelationDelay (kvc::LoadedPlugin& plugin, int blockSize)
    {
        const int total = 48000;                     // 1 秒
        std::vector<float> input ((size_t) total, 0.0f);
        std::vector<float> output ((size_t) total, 0.0f);

        // 200 Hz の連続正弦（途中で振幅を跳ね上げて目印にする）
        for (int i = 0; i < total; ++i)
        {
            const double t = (double) i / kRate;
            const double env = (i > total / 2 && i < total / 2 + 2400) ? 1.0 : 0.15;
            input[(size_t) i] = (float) (env * 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * t));
        }

        std::vector<float> buf ((size_t) blockSize);

        for (int pos = 0; pos + blockSize <= total; pos += blockSize)
        {
            std::copy (input.begin() + pos, input.begin() + pos + blockSize, buf.begin());
            plugin.processMono (buf.data(), blockSize);
            std::copy (buf.begin(), buf.end(), output.begin() + pos);
        }

        // 振幅が跳ねた位置を入力・出力それぞれで探す
        const auto findJump = [total] (const std::vector<float>& v)
        {
            const int from = total / 2 - 4800;
            float base = 0.0f;

            for (int i = from; i < total / 2; ++i)
                base = juce::jmax (base, std::abs (v[(size_t) i]));

            for (int i = total / 2 - 480; i < total - 1; ++i)
                if (std::abs (v[(size_t) i]) > base * 2.5f)
                    return i;

            return -1;
        };

        const int a = findJump (input);
        const int b = findJump (output);

        return (a >= 0 && b >= 0) ? b - a : -1;
    }
}

class DiagApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "PluginLatencyProbe"; }
    const juce::String getApplicationVersion() override { return "1.0"; }

    void initialise (const juce::String&) override
    {
        const juce::File dll ("C:\\Program Files\\Common Files\\VST2\\pitchproof-x64.dll");

        line ("=== Pitchproof 遅延の実測 ===");
        line ("");

        for (const int block : { 64, 128, 256 })
        {
            juce::String err;
            auto plugin = kvc::createVst2Plugin (dll, err);

            if (plugin == nullptr)
            {
                line ("読み込み失敗: " + err);
                break;
            }

            plugin->prepare (kRate, block);

            const int claimed = plugin->getLatencySamples();
            const int impulse = measureImpulseDelay (*plugin, block, 1.0e-4f);

            plugin->reset();
            const int corr = measureCorrelationDelay (*plugin, block);

            line ("ブロック " + juce::String (block).paddedLeft (' ', 3)
                  + " | 申告 " + juce::String (claimed).paddedLeft (' ', 5) + " smp ("
                  + juce::String (1000.0 * claimed / kRate, 2) + " ms)"
                  + " | インパルス実測 " + juce::String (impulse).paddedLeft (' ', 5) + " smp ("
                  + juce::String (impulse >= 0 ? 1000.0 * impulse / kRate : -1.0, 2) + " ms)"
                  + " | 相関実測 " + juce::String (corr).paddedLeft (' ', 5) + " smp ("
                  + juce::String (corr >= 0 ? 1000.0 * corr / kRate : -1.0, 2) + " ms)");
        }

        juce::File ("D:\\vcdev\\plugin_latency.txt").replaceWithText (reportText);
        quit();
    }

    void shutdown() override {}
};

START_JUCE_APPLICATION (DiagApp)
