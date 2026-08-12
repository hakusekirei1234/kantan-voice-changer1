// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "platform/RevealInExplorer.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

namespace kvc::platform
{

namespace
{

/** ShellExecuteW は成功時に 32 より大きい値を返す（歴史的な仕様）。 */
bool shellExecuteSucceeded (HINSTANCE r) noexcept
{
    return reinterpret_cast<INT_PTR> (r) > 32;
}

bool openPathWithShell (const juce::String& path)
{
    if (path.isEmpty())
        return false;

    // toWideCharPointer() の戻り値は元の juce::String の寿命に紐づく。
    // 一時オブジェクトに対して呼ぶとダングリングするのでローカルに束ねる。
    const juce::String held (path);

    const auto r = ShellExecuteW (nullptr, L"open", held.toWideCharPointer(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
    return shellExecuteSucceeded (r);
}

/** SHOpenFolderAndSelectItems が失敗したときだけ使う最後の手段。
    explorer.exe のコマンドラインはカンマを含むパスで壊れる（引用符でも守れない）ので、
    主経路にしてはいけない。 */
bool revealViaExplorerCommandLine (const juce::File& file)
{
    const auto fullPath = file.getFullPathName();

    if (fullPath.containsChar (','))
        return false;

    const juce::String args = "/select,\"" + fullPath + "\"";

    const auto r = ShellExecuteW (nullptr, L"open", L"explorer.exe",
                                  args.toWideCharPointer(), nullptr, SW_SHOWNORMAL);
    return shellExecuteSucceeded (r);
}

} // namespace

//==============================================================================
bool revealFileInExplorer (const juce::File& file)
{
    if (file == juce::File())
        return false;

    if (! file.exists())
    {
        // 消えていても「書き出し失敗」には見せない。親フォルダだけ開いて終わる。
        openFolder (file.getParentDirectory());
        return false;
    }

    const auto fullPath = file.getFullPathName();

    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attributes = 0;

    // ドキュメント上、SHOpenFolderAndSelectItems の前に CoInitialize[Ex] が必須。
    // JUCE のメッセージスレッドは OleInitialize 済み（STA）なので、ここでは初期化しない。
    const HRESULT parsed = SHParseDisplayName (fullPath.toWideCharPointer(), nullptr,
                                               &pidl, 0, &attributes);

    if (SUCCEEDED (parsed) && pidl != nullptr)
    {
        // cidl==0 / apidl==nullptr は「pidlFolder を単一アイテムとして扱う」指定。
        // エクスプローラが親フォルダを開いてこのファイルを選択状態にする。
        const HRESULT opened = SHOpenFolderAndSelectItems (pidl, 0, nullptr, 0);

        CoTaskMemFree (pidl);   // SHParseDisplayName の PIDL は ILFree ではなくこちら

        if (SUCCEEDED (opened))
            return true;
    }
    else if (pidl != nullptr)
    {
        CoTaskMemFree (pidl);
    }

    if (revealViaExplorerCommandLine (file))
        return true;

    openFolder (file.getParentDirectory());
    return false;
}

//==============================================================================
bool openFolder (const juce::File& folder)
{
    if (folder == juce::File() || ! folder.isDirectory())
        return false;

    return openPathWithShell (folder.getFullPathName());
}

//==============================================================================
bool openSettingsUri (const juce::String& uri)
{
    return openPathWithShell (uri);
}

} // namespace kvc::platform
