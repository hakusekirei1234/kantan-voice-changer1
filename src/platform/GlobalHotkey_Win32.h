// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>

namespace kvc
{

//==============================================================================
/** 修飾キーのビット。値は Win32 の MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN と
    同一なので、実装側はそのまま RegisterHotKey に渡せる。
    ヘッダに windows.h を持ち込まないための定義。 */
namespace HotkeyMods
{
    inline constexpr uint32_t none    = 0;
    inline constexpr uint32_t alt     = 0x0001;
    inline constexpr uint32_t control = 0x0002;
    inline constexpr uint32_t shift   = 0x0004;
    inline constexpr uint32_t win     = 0x0008;
}

/** JIS 配列で「安全な単独キー」として勧められる VK。 */
namespace SafeKeys
{
    inline constexpr uint32_t vkNonConvert = 0x1D;  ///< 無変換
    inline constexpr uint32_t vkConvert    = 0x1C;  ///< 変換
    inline constexpr uint32_t vkPause      = 0x13;
    inline constexpr uint32_t vkF9         = 0x78;
    inline constexpr uint32_t vkF12        = 0x7B;
}

enum class HotkeyMode
{
    toggle = 0,     ///< 押すたびに切り替え
    pushToMute = 1  ///< 押している間だけミュート（安全側に倒れる方向）
};

struct HotkeyBinding
{
    uint32_t   vk = 'M';
    uint32_t   mods = HotkeyMods::control | HotkeyMods::shift;   ///< 既定 Ctrl+Shift+M
    HotkeyMode mode = HotkeyMode::toggle;
    /** 他のアプリにもキーを通すか。単独キー時は既定 true にしないと
        そのキーが Windows 全体で使えなくなる。 */
    bool passThrough = true;

    bool operator== (const HotkeyBinding& o) const noexcept
    {
        return vk == o.vk && mods == o.mods && mode == o.mode && passThrough == o.passThrough;
    }
};

//==============================================================================
/** 2 バックエンド構成のグローバルホットキー。

    バックエンドの選び方（ユーザーの要求からバックエンドを決める。逆はしない）:
        修飾キーあり かつ トグル かつ 非パススルー → RegisterHotKey
        それ以外                                   → WH_KEYBOARD_LL

    RegisterHotKey:
      - OS が調停するのでフックチェーンに入らず、他アプリの入力経路に居座らない。
      - 必ず MOD_NOREPEAT を付ける。付けないと押しっぱなしでキーリピートし、
        ミュートがキーリピート速度でパタパタする。
      - 失敗（ERROR_HOTKEY_ALREADY_REGISTERED）を必ず検出して日本語で出す。
      - キーアップが取れないのでプッシュトゥミュートは実装不能。
      - WM_HOTKEY は hwnd==NULL のスレッドメッセージだと JUCE の DispatchMessage に
        捨てられる。HWND_MESSAGE のメッセージ専用ウィンドウを作って登録すること。

    WH_KEYBOARD_LL:
      - キーアップも見えるのでプッシュトゥミュートが可能。
      - CallNextHookEx で「消費せずに観測」できる。単独キーを潰さずに割り当てる
        唯一の方法。
      - ★フックプロシージャは atomic 2 個への store と PostMessage だけ。
        LowLevelHooksTimeout（既定 300 ms）を超えると Windows が黙ってフックを
        呼ばなくなる。ロック・確保・ログ・GUI 呼び出しは一切禁止。
      - 修飾キーの一致判定は GetAsyncKeyState で毎回取る。自前で状態を追うと
        フォーカス変化でずれる。
      - 5 秒ごとのウォッチドッグでフックの生死を確認し、死んでいたら再インストールする。
      - プッシュトゥミュート時は 250 ms ごとに GetAsyncKeyState で実際の押下状態と
        照合し、取りこぼしたキーアップを強制解放する（昇格ウィンドウや UAC で
        キーアップが見えない）。セッションロックでも強制解放する。

    スレッド契約:
      install / uninstall / addListener / removeListener ... メッセージスレッド専用
      Listener のコールバック                              ... メッセージスレッドで呼ばれる
      静的ヘルパ                                           ... どこからでも
*/
class GlobalHotkey : private juce::Timer
{
public:
    enum class BackendKind
    {
        none = 0,
        registerHotKey,
        lowLevelHook
    };

    enum class InstallResult
    {
        ok = 0,
        alreadyRegisteredByAnotherApp,   ///< ERROR_HOTKEY_ALREADY_REGISTERED
        hookInstallFailed,
        invalidBinding                   ///< Enter / Space 単独など
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** キーダウン。トグルモードならここで切り替える。 */
        virtual void hotkeyPressed() = 0;
        /** キーアップ。RegisterHotKey バックエンドでは呼ばれない。 */
        virtual void hotkeyReleased() {}
        /** ウォッチドッグがフックを再インストールした。1 分に 2 回起きたら UI に出す。 */
        virtual void hotkeyBackendRecovered() {}
    };

    GlobalHotkey();
    ~GlobalHotkey() override;

    InstallResult install (const HotkeyBinding&);
    void uninstall();

    bool           isInstalled() const noexcept { return backend != BackendKind::none; }
    BackendKind    getActiveBackend() const noexcept { return backend; }
    HotkeyBinding  getBinding() const noexcept { return binding; }

    /** プッシュトゥミュート時に、いま押されていると認識しているか。 */
    bool isKeyHeld() const noexcept;

    void addListener (Listener*);
    void removeListener (Listener*);

    //==========================================================================
    static BackendKind chooseBackend (const HotkeyBinding&) noexcept;

    /** JIS 配列で正しく出る表示名。0x1D → 「無変換」、0x1C → 「変換」、
        それ以外は GetKeyNameTextW / MapVirtualKeyW。生の 0x1D を出さないこと。 */
    static juce::String describeBinding (const HotkeyBinding&);
    static juce::String describeKey (uint32_t vk);

    /** 修飾キーなしの文字・数字キー。UI で警告を出すべき組み合わせ。 */
    static bool isRiskyBareKey (const HotkeyBinding&) noexcept;

    /** Enter / Space 単独など、そもそも割り当てさせないもの。 */
    static bool isForbiddenBinding (const HotkeyBinding&) noexcept;

    static juce::String japaneseError (InstallResult);

private:
    void timerCallback() override;

    HotkeyBinding binding;
    BackendKind   backend = BackendKind::none;

    juce::ListenerList<Listener> listeners;

    struct Impl;                 // メッセージ専用ウィンドウ・フックハンドル一式
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlobalHotkey)
};

//==============================================================================
/** キー割当ダイアログ用の一時キャプチャ。

    juce::Component::keyPressed では Win 組み合わせ・PrintScreen・無変換が
    確実に取れず、Tab や矢印キーはフォーカス移動に食われる。専用の一時 LL フックを
    張り、Esc 以外は全部消費する（キャプチャ中の打鍵が他アプリに漏れないように）。

    スレッド契約: 全てメッセージスレッド専用。
*/
class HotkeyCapture
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void keyCaptured (uint32_t vk, uint32_t mods) = 0;
        virtual void captureCancelled() = 0;
    };

    HotkeyCapture();
    ~HotkeyCapture();

    /** 呼び出し側は事前に本番のバインドを uninstall しておくこと
        （キャプチャ中に古いキーが発火しないように）。 */
    bool begin (Listener*);
    void end();
    bool isActive() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotkeyCapture)
};

} // namespace kvc
