// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include "core/DriftRing.h"

#include <string>

using namespace kvctest;

namespace
{
    /** マスターとスレーブを交互に回して、長時間のドリフトを模擬する。

        @param clockErrorPpm  スレーブの実クロックがマスターより速い/遅い量。
                              実機では別々の水晶なので必ず数十〜数百 ppm ずれる。 */
    struct DriftResult
    {
        kvc::DriftRing::Diagnostics diagnostics;
        bool finiteOutput = true;
    };

    DriftResult runDrift (double clockErrorPpm, double seconds,
                          int masterBlock = 128, int slaveBlock = 128)
    {
        kvc::DriftRing ring;
        ring.prepare (1, 48000.0, 48000.0, masterBlock, slaveBlock,
                      kvc::DriftRing::FillMode::stable);
        ring.prefillSilence();

        std::vector<float> masterBuffer (static_cast<size_t> (masterBlock), 0.0f);
        std::vector<float> slaveBuffer (static_cast<size_t> (slaveBlock), 0.0f);

        float* masterChannels[1] = { masterBuffer.data() };
        float* slaveChannels[1]  = { slaveBuffer.data() };

        const double totalMasterFrames = 48000.0 * seconds;

        // スレーブのクロック誤差は「同じ実時間でスレーブが要求するフレーム数の差」として現れる。
        const double slaveFrames = totalMasterFrames * (1.0 + clockErrorPpm * 1.0e-6);

        double masterDone = 0.0, slaveDone = 0.0;
        double phase = 0.0;

        DriftResult result;

        while (masterDone < totalMasterFrames)
        {
            for (int i = 0; i < masterBlock; ++i)
            {
                masterBuffer[static_cast<size_t> (i)] = 0.5f * static_cast<float> (std::sin (phase));
                phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0;
            }

            const float* const* source = masterChannels;
            ring.write (source, 1, masterBlock);
            masterDone += masterBlock;

            // マスターの進みに追いつくまでスレーブを回す。
            while (slaveDone / slaveFrames < masterDone / totalMasterFrames)
            {
                ring.read (slaveChannels, 1, slaveBlock);
                slaveDone += slaveBlock;

                if (! allFinite (slaveBuffer.data(), slaveBlock))
                    result.finiteOutput = false;
            }
        }

        result.diagnostics = ring.getDiagnostics();
        return result;
    }
}

//==============================================================================

KVC_TEST (DriftRing, target_fill_follows_the_documented_formula)
{
    using Ring = kvc::DriftRing;

    KVC_CHECK (Ring::computeTargetFill (128, 128, Ring::FillMode::lowLatency)
                   >= 128 + 128 + 32);
    KVC_CHECK (Ring::computeTargetFill (128, 128, Ring::FillMode::stable)
                   >= Ring::computeTargetFill (128, 128, Ring::FillMode::lowLatency));

    // どちらの式でも [256, kCapacity/4] のクランプから出ないこと。
    for (int master : { 16, 64, 128, 512, 2048 })
    {
        for (int slave : { 16, 64, 128, 512, 2048 })
        {
            for (auto mode : { Ring::FillMode::stable, Ring::FillMode::lowLatency })
            {
                const int fill = Ring::computeTargetFill (master, slave, mode);
                KVC_CHECK_IN_RANGE (fill, 256, Ring::kCapacity / 4);
            }
        }
    }
}

KVC_TEST (DriftRing, matched_clocks_never_underrun)
{
    const auto result = runDrift (0.0, 20.0);

    note ("ppm " + std::to_string (result.diagnostics.ppm)
              + ", underruns " + std::to_string (result.diagnostics.underruns)
              + ", overruns " + std::to_string (result.diagnostics.overruns));

    KVC_CHECK (result.finiteOutput);
    KVC_CHECK (result.diagnostics.underruns == 0);
    KVC_CHECK (result.diagnostics.overruns == 0);
}

KVC_TEST (DriftRing, absorbs_a_realistic_clock_mismatch)
{
    // 別々の水晶なら 100〜500 ppm はふつうに出る。
    // ここで吸収できないと，数分に一度プチッと鳴る。
    for (double ppm : { -300.0, -100.0, 100.0, 300.0 })
    {
        const auto result = runDrift (ppm, 30.0);

        note (std::to_string (ppm) + " ppm -> corrected "
                  + std::to_string (result.diagnostics.ppm)
                  + " ppm, underruns " + std::to_string (result.diagnostics.underruns)
                  + ", overruns " + std::to_string (result.diagnostics.overruns));

        KVC_CHECK (result.finiteOutput);
        KVC_CHECK (result.diagnostics.underruns == 0);
        KVC_CHECK (result.diagnostics.overruns == 0);
        KVC_CHECK (! result.diagnostics.pinnedAtClamp);
    }
}

KVC_TEST (DriftRing, mismatched_block_sizes_stay_stable)
{
    const auto result = runDrift (50.0, 20.0, 128, 480);

    note ("fill " + std::to_string (result.diagnostics.fill)
              + " / target " + std::to_string (result.diagnostics.targetFill)
              + ", underruns " + std::to_string (result.diagnostics.underruns));

    KVC_CHECK (result.finiteOutput);
    KVC_CHECK (result.diagnostics.underruns == 0);
}
