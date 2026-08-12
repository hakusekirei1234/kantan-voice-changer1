// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_core/juce_core.h>

namespace kvc::platform
{

/** SHParseDisplayName + SHOpenFolderAndSelectItems で、親フォルダを開いて
    そのファイルを選択状態にする。

    ★COM が初期化済みのスレッドから呼ぶこと。
      JUCE のメッセージスレッドは OleInitialize 済み（STA）なので、そこから呼ぶ。
      Mp3Exporter の MTA スレッドから呼んではいけない。書き出し完了は
      MessageManager 経由でメッセージスレッドへ渡してから reveal する。

    ファイルは呼び出し時点で実在していること（Finalize と MoveFileExW の後）。
    失敗したら親フォルダを開くだけのフォールバックへ落ちる。
    reveal の失敗を「書き出し失敗」として見せないこと。

    @returns 選択表示まで成功したら true。 */
bool revealFileInExplorer (const juce::File& file);

/** フォルダを開くだけ。reveal のフォールバックと「ログフォルダを開く」用。 */
bool openFolder (const juce::File& folder);

/** ms-settings: 等の設定 URI を開く。マイクのプライバシー設定へ誘導するのに使う。 */
bool openSettingsUri (const juce::String& uri);

} // namespace kvc::platform
