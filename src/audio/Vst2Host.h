// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include "audio/PluginHost.h"

namespace kvc
{

/** VST2 プラグインを読み込む。失敗したら nullptr と日本語のエラーを返す。
    メッセージスレッド専用。KVC_ENABLE_VST2=ON のビルドでのみリンクされる。 */
std::unique_ptr<LoadedPlugin> createVst2Plugin (const juce::File& dll, juce::String& errorJapanese);

} // namespace kvc
