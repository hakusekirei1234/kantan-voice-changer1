// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "TestSupport.h"

#include <cstring>
#include <exception>

//==============================================================================
// 使い方:
//   kvc_tests               すべて実行
//   kvc_tests <部分一致>    スイート名かテスト名に含まれるものだけ実行
//
// 戻り値は失敗件数。CI はこれだけを見る。
//==============================================================================
int main (int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;

    auto& cases = kvctest::registry();

    int passed = 0, failed = 0, skipped = 0;
    const char* currentSuite = nullptr;

    std::printf ("\n=== kantan voice changer :: DSP tests ===\n\n");

    for (const auto& test : cases)
    {
        if (filter != nullptr
            && std::strstr (test.suite, filter) == nullptr
            && std::strstr (test.name, filter) == nullptr)
        {
            ++skipped;
            continue;
        }

        if (currentSuite == nullptr || std::strcmp (currentSuite, test.suite) != 0)
        {
            currentSuite = test.suite;
            std::printf ("[%s]\n", currentSuite);
        }

        std::printf ("  %-46s ", test.name);
        std::fflush (stdout);

        try
        {
            test.run();
            std::printf ("ok\n");
            ++passed;
        }
        catch (const kvctest::Failure& failure)
        {
            std::printf ("FAILED\n      %s\n", failure.message.c_str());
            ++failed;
        }
        catch (const std::exception& e)
        {
            std::printf ("FAILED (exception)\n      %s\n", e.what());
            ++failed;
        }
        catch (...)
        {
            std::printf ("FAILED (unknown exception)\n");
            ++failed;
        }

        std::fflush (stdout);
    }

    std::printf ("\n----------------------------------------\n");
    std::printf ("passed %d / failed %d", passed, failed);

    if (skipped > 0)
        std::printf (" / skipped %d", skipped);

    std::printf ("\n\n");

    return failed;
}
