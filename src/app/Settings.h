// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include "audio/Mp3Exporter.h"
#include "core/DriftRing.h"
#include "core/Parameters.h"
#include "platform/GlobalHotkey_Win32.h"

#include <memory>

namespace kvc
{

//==============================================================================
/** %APPDATA%\SimpleVoiceChanger\settings.xml。

    - フォルダ名は ASCII のみ（CP932 のフォルダ名は面倒を呼ぶ）。
    - exe の隣にも D:\SynologyDrive 配下にも置かない。同期クライアントが
      settings (conflict).xml を作るとサポート不能になる。
    - デバイスは必ずエンドポイント ID で保存する。名前保存は
      「MOTU M Series (2)」のズレで壊れる。
    - 書き込みは 1 秒デバウンス。スライダーのドラッグで毎秒数百回 XML を
      書き直さないため。shutdown と WM_ENDSESSION では無条件に保存する。
    - パース失敗時は settings.bad.xml にリネームして既定値で起動する。
      壊れた設定で起動できなくなることが絶対に無いようにする。

    スレッド契約: 全メソッドがメッセージスレッド専用。
*/
class Settings : private juce::Timer
{
public:
    static constexpr int kSettingsVersion = 1;
    static constexpr int kSaveDebounceMs = 1000;

    Settings();
    ~Settings() override;

    /** 起動時に 1 回。失敗しても例外は投げず、既定値に落ちる。 */
    void load();

    /** デバウンスを待たずに今すぐ書く。shutdown / WM_ENDSESSION から。 */
    void saveIfNeeded();

    /** 値を変えたあとに呼ぶ。1 秒後にまとめて保存される。 */
    void scheduleSave();

    /** 設定ファイルを消して既定値に戻す。詳細設定→情報の「設定をリセット」。 */
    void resetToDefaults();

    juce::File getSettingsFolder() const;

    /** settings.xml が存在しなかった = 初回起動。誘導状態を出す判断に使う。 */
    bool isFirstRun() const noexcept { return firstRun; }

    //==========================================================================
    // デバイス（エンドポイント ID）

    juce::String getInputEndpointId() const;
    juce::String getMonitorEndpointId() const;
    juce::String getSendEndpointId() const;
    void setInputEndpointId (const juce::String&);
    void setMonitorEndpointId (const juce::String&);
    void setSendEndpointId (const juce::String&);

    //==========================================================================
    // オーディオ

    int  getRequestedBlockSize() const;          ///< 0 = 自動
    void setRequestedBlockSize (int);
    double getRequestedSampleRate() const;       ///< 0 = 自動
    void setRequestedSampleRate (double);
    bool getAllowAsio() const;
    void setAllowAsio (bool);
    bool getAllowExclusive() const;
    void setAllowExclusive (bool);
    DriftRing::FillMode getFillMode() const;     ///< 低遅延優先 / 安定優先
    void setFillMode (DriftRing::FillMode);

    /** 使う入力チャンネル。-1 = 先頭 2 本をミックス、0 以上 = そのチャンネル 1 本。
        既定 0。オーディオインターフェースは入力を何本も出すので、
        「先頭 2 本固定」では In 3 以降のマイクが拾えない。 */
    int  getInputChannelIndex() const;
    void setInputChannelIndex (int);

    //==========================================================================
    // 声

    float getPitchSemitones() const;      void setPitchSemitones (float);
    float getFormantSemitones() const;    void setFormantSemitones (float);
    bool  getPitchEnabled() const;        void setPitchEnabled (bool);
    bool  getFormantEnabled() const;      void setFormantEnabled (bool);
    bool  getNoiseSuppressionEnabled() const;  void setNoiseSuppressionEnabled (bool);
    NoiseStrength getNoiseStrength() const;    void setNoiseStrength (NoiseStrength);
    NoiseQuality  getNoiseQuality() const;     void setNoiseQuality (NoiseQuality);
    float getInputGainDb() const;         void setInputGainDb (float);
    float getOutputGainDb() const;        void setOutputGainDb (float);
    bool  getMuteAlsoSilencesMonitor() const;  void setMuteAlsoSilencesMonitor (bool);

    //==========================================================================
    // 音声処理エンジン（内蔵 PSOLA / 外部プラグイン）

    enum class EngineKind { builtin = 0, plugin = 1 };

    EngineKind getEngineKind() const;              void setEngineKind (EngineKind);
    juce::String getPluginIdentifier() const;      void setPluginIdentifier (const juce::String&);

    /** プラグイン自身の設定。Base64 で持つ。 */
    juce::MemoryBlock getPluginState() const;      void setPluginState (const juce::MemoryBlock&);

    //==========================================================================
    // 声質判定

    /** 判定テキストを切り替える間隔（ミリ秒）。既定 1000。 */
    int  getCharacterIntervalMs() const;           void setCharacterIntervalMs (int);
    bool getCharacterEnabled() const;              void setCharacterEnabled (bool);

    //==========================================================================
    // 表示

    /** 画面全体の拡大率。1.0〜1.5。juce::Desktop の全体スケールへそのまま渡す。 */
    float getUiScale() const;                      void setUiScale (float);

    //==========================================================================
    // ホットキー・通知音

    HotkeyBinding getHotkeyBinding() const;
    void setHotkeyBinding (const HotkeyBinding&);
    bool  getBeepEnabled() const;   void setBeepEnabled (bool);
    float getBeepVolume() const;    void setBeepVolume (float);

    //==========================================================================
    // 書き出し

    Mp3Exporter::Format getExportFormat() const;   void setExportFormat (Mp3Exporter::Format);
    int  getMp3BitrateKbps() const;                void setMp3BitrateKbps (int);
    juce::File getExportFolder() const;            void setExportFolder (const juce::File&);

    //==========================================================================
    // ウィンドウ

    /** 保存が無ければ空の矩形。復元側でディスプレイ範囲にクランプすること。 */
    juce::Rectangle<int> getWindowBounds() const;
    void setWindowBounds (juce::Rectangle<int>);

    //==========================================================================
    /** 保存値を VoiceParams へ流し込む（起動時に 1 回）。 */
    void applyToParams (VoiceParams&) const;
    /** VoiceParams の現在値を保存値へ取り込む（終了時など）。 */
    void captureFromParams (const VoiceParams&);

private:
    void timerCallback() override;
    juce::PropertiesFile* props() const;

    std::unique_ptr<juce::ApplicationProperties> appProps;
    bool firstRun = false;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Settings)
};

} // namespace kvc
