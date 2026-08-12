// SPDX-License-Identifier: AGPL-3.0-or-later
// 簡単ボイチェン (Kantan Voice Changer)
// This file is part of a program licensed under the GNU AGPL v3 or later.
// See LICENSE.txt for the full licence text.

#include "ui/DeviceSelectorPanel.h"

#include <algorithm>
#include <vector>

namespace kvc
{

namespace
{
    inline juce::String jp (const char* utf8) { return juce::String::fromUTF8 (utf8); }

    const juce::Colour kPanel   { 0xff1e2227 };
    const juce::Colour kText    { 0xffe8eaed };
    const juce::Colour kDim     { 0xff8a9099 };
    const juce::Colour kAccent  { 0xff2ed573 };
    const juce::Colour kDanger  { 0xffff4757 };
    const juce::Colour kAmber   { 0xffffa502 };
    const juce::Colour kField   { 0xff14171a };

    //==========================================================================
    // ComboBox の項目 ID から エンドポイント ID を引く対応表は、Component の
    // NamedValueSet に持たせる。ヘッダに専用のメンバ配列を足さずに、
    // 「表示した瞬間の対応」をウィジェット自身に固定できる。カタログが
    // 作り直されても、次の populate まで古い対応が壊れない。
    const juce::Identifier kDesiredIdKey { "kvcDesiredId" };

    juce::Identifier itemKey (int itemId)
    {
        return juce::Identifier ("kvcItem" + juce::String (itemId));
    }

    /** 選択なしのときにコンボへ出す文言。missing の方は「前回の機器が消えた」印にも使う
        （黙って別の機器に差し替えないための状態を、ウィジェット側に持たせている）。 */
    const char* const kNothingSelectedText = "（選んでください）";
    const char* const kMissingMarkerText   = "\xe2\x9a\xa0 前回の機器が見つかりません";

    constexpr int kSendNoneItemId = 1;

    //==========================================================================
    struct Row
    {
        const DeviceEntry* entry = nullptr;
        Backend badge = Backend::unknown;
        int group = 0;      ///< 0 = おすすめ / ピン留め, 1 = そのほか
        int rank = 0;
    };

    /** 詳細設定が OFF のあいだは 排他 と ASIO の行を出さない。どちらも
        SYNCROOM との共存を壊すので、非技術者に踏ませてはいけない。 */
    Backend visibleBackendFor (const DeviceEntry& e, bool advanced) noexcept
    {
        if (advanced)
            return e.bestBackend;

        // 遅延最優先。排他だけは非表示のまま（SYNCROOM との共存を確実に壊すため）。
        if (e.availableBackends.contains (Backend::asio))
            return Backend::asio;

        if (e.availableBackends.contains (Backend::wasapiLowLatency))
            return Backend::wasapiLowLatency;

        if (e.availableBackends.contains (Backend::wasapiShared))
            return Backend::wasapiShared;

        return Backend::unknown;
    }

    std::vector<Row> buildRows (const DeviceCatalog& catalog,
                                DeviceSelectorPanel::Column column,
                                bool advanced)
    {
        using Column = DeviceSelectorPanel::Column;

        const auto& source = (column == Column::input) ? catalog.getInputs()
                                                       : catalog.getOutputs();

        std::vector<Row> rows;
        rows.reserve ((size_t) source.size());

        for (const auto& e : source)
        {
            const auto badge = visibleBackendFor (e, advanced);

            if (badge == Backend::unknown)
                continue;

            Row r;
            r.entry = &e;
            r.badge = badge;

            switch (column)
            {
                case Column::input:
                    r.group = 1;
                    r.rank = e.isDefaultCommunications ? 0 : (e.isDefaultConsole ? 1 : (e.isVirtual ? 3 : 2));
                    break;

                case Column::monitor:
                    r.group = 1;
                    r.rank = e.isDefaultConsole ? 0 : (e.isVirtual ? 2 : 1);
                    break;

                case Column::send:
                default:
                    // 送信先候補（SYNCROOM Driver / VoiceMeeter / CABLE）を先頭にピン留めする。
                    r.group = e.sendPriority > 0 ? 0 : 1;
                    r.rank  = e.sendPriority > 0 ? -e.sendPriority : (e.isVirtual ? 0 : 1);
                    break;
            }

            rows.push_back (r);
        }

        std::stable_sort (rows.begin(), rows.end(),
                          [] (const Row& a, const Row& b)
                          {
                              if (a.group != b.group) return a.group < b.group;
                              return a.rank < b.rank;
                          });

        return rows;
    }

    //==========================================================================
    juce::String endpointIdForItem (const juce::ComboBox& combo, int itemId)
    {
        if (itemId == 0)
            return {};

        return combo.getProperties().getWithDefault (itemKey (itemId), juce::String()).toString();
    }

    juce::String selectedEndpointId (const juce::ComboBox& combo)
    {
        return endpointIdForItem (combo, combo.getSelectedId());
    }

    bool isMarkedMissing (const juce::ComboBox& combo)
    {
        return combo.getSelectedId() == 0
            && combo.getTextWhenNothingSelected() == jp (kMissingMarkerText);
    }

    //==========================================================================
    void populateCombo (juce::ComboBox& combo,
                        const DeviceCatalog& catalog,
                        DeviceSelectorPanel::Column column,
                        bool advanced)
    {
        using Column = DeviceSelectorPanel::Column;

        // 自分が置いた対応表だけを外す。NamedValueSet を丸ごと clear() すると
        // JUCE 側が付けたプロパティまで巻き添えになる。
        for (int i = 0; i < combo.getNumItems(); ++i)
            combo.getProperties().remove (itemKey (combo.getItemId (i)));

        combo.clear (juce::dontSendNotification);

        int nextId = 1;

        if (column == Column::send)
        {
            // 「送信しない」は一級の選択肢。相手に送らずモニターだけ、が普通に有り得る。
            combo.addItem (jp ("送信しない（モニターのみ）"), kSendNoneItemId);
            combo.getProperties().set (itemKey (kSendNoneItemId), juce::String());
            nextId = kSendNoneItemId + 1;
        }

        const auto rows = buildRows (catalog, column, advanced);

        const bool anyPinned = std::any_of (rows.begin(), rows.end(),
                                            [] (const Row& r) { return r.group == 0; });
        const bool anyOther  = std::any_of (rows.begin(), rows.end(),
                                            [] (const Row& r) { return r.group != 0; });

        juce::StringArray usedNames;
        int lastGroup = -1;

        for (const auto& row : rows)
        {
            if (column == Column::send && row.group != lastGroup)
            {
                if (row.group == 0)
                {
                    combo.addSeparator();
                    combo.addSectionHeading (jp ("おすすめ（SYNCROOM など）"));
                }
                else if (anyPinned && anyOther)
                {
                    combo.addSeparator();
                    combo.addSectionHeading (jp ("そのほか"));
                }
            }

            lastGroup = row.group;

            auto text = backendBadge (row.badge) + " " + row.entry->friendlyName;

            // 同名のエンドポイントが並ぶ機械がある（MOTU M Series が 3 つ等）。
            // 同じ行が 2 つ見えるほうが、番号が付くより混乱する。
            if (usedNames.contains (text))
                text += " #" + juce::String (usedNames.size() + 1);

            usedNames.add (text);

            combo.addItem (text, nextId);
            combo.getProperties().set (itemKey (nextId), row.entry->endpointId);
            ++nextId;
        }
    }

    /** 保存済み／直前の選択をコンボへ戻す。見つからない ID は選択なしにして
        印だけ残す。黙って別の機器に差し替えるのが最悪の挙動なので、それはしない。 */
    void applyDesiredSelection (juce::ComboBox& combo, DeviceSelectorPanel::Column column)
    {
        using Column = DeviceSelectorPanel::Column;

        const auto desired = combo.getProperties()
                                  .getWithDefault (kDesiredIdKey, juce::String()).toString();

        if (desired.isEmpty())
        {
            combo.setTextWhenNothingSelected (jp (kNothingSelectedText));
            combo.setSelectedId (column == Column::send ? kSendNoneItemId : 0,
                                 juce::dontSendNotification);
            return;
        }

        for (int i = 0; i < combo.getNumItems(); ++i)
        {
            const int id = combo.getItemId (i);

            if (id != 0 && endpointIdForItem (combo, id) == desired)
            {
                combo.setTextWhenNothingSelected (jp (kNothingSelectedText));
                combo.setSelectedId (id, juce::dontSendNotification);
                return;
            }
        }

        combo.setTextWhenNothingSelected (jp (kMissingMarkerText));
        combo.setSelectedId (0, juce::dontSendNotification);
    }

    //==========================================================================
    /** 選べない組み合わせは事後にエラーを出すのでは遅い。非技術者が踏むと
        ヘッドホンに全音量のハウリングが返る。ここで灰色にして踏めなくする。 */
    void applyAvailability (juce::ComboBox (&combos)[3], const DeviceCatalog& catalog)
    {
        const auto inputId   = selectedEndpointId (combos[0]);
        const auto monitorId = selectedEndpointId (combos[1]);
        const auto sendId    = selectedEndpointId (combos[2]);

        const DeviceEntry* inputEntry = inputId.isNotEmpty() ? catalog.findInput  (inputId) : nullptr;
        const DeviceEntry* sendEntry  = sendId.isNotEmpty()  ? catalog.findOutput (sendId)  : nullptr;

        bool anyBlocked = false;

        for (int i = 0; i < combos[0].getNumItems(); ++i)
        {
            const int id = combos[0].getItemId (i);
            const auto candidate = endpointIdForItem (combos[0], id);
            bool ok = true;

            if (sendId.isNotEmpty())
            {
                if (candidate == sendId)
                    ok = false;
                else if (sendEntry != nullptr)
                    if (auto* e = catalog.findInput (candidate))
                        ok = ! catalog.isCaptureSideOf (*e, *sendEntry);
            }

            combos[0].setItemEnabled (id, ok);
            anyBlocked = anyBlocked || ! ok;
        }

        for (int i = 0; i < combos[1].getNumItems(); ++i)
        {
            const int id = combos[1].getItemId (i);
            const bool ok = ! (sendId.isNotEmpty() && endpointIdForItem (combos[1], id) == sendId);

            combos[1].setItemEnabled (id, ok);
            anyBlocked = anyBlocked || ! ok;
        }

        for (int i = 0; i < combos[2].getNumItems(); ++i)
        {
            const int id = combos[2].getItemId (i);
            const auto candidate = endpointIdForItem (combos[2], id);

            if (candidate.isEmpty())            // 「送信しない」は常に選べる
            {
                combos[2].setItemEnabled (id, true);
                continue;
            }

            bool ok = true;

            if (inputId.isNotEmpty() && candidate == inputId)
                ok = false;

            if (ok && monitorId.isNotEmpty() && candidate == monitorId)
                ok = false;

            if (ok && inputEntry != nullptr)
                if (auto* o = catalog.findOutput (candidate))
                    ok = ! catalog.isCaptureSideOf (*inputEntry, *o);

            combos[2].setItemEnabled (id, ok);
            anyBlocked = anyBlocked || ! ok;
        }

        const auto suffix = anyBlocked
            ? jp ("\n灰色の機器は、いまの組み合わせでは選べません（ハウリングの原因になります）。")
            : juce::String();

        static const char* const baseTips[3] =
        {
            "あなたの声を拾うマイクを選びます",
            "あなたが自分の声を確認するための出力先です",
            "変換した声を送る先です。SYNCROOM で使うなら「Yamaha SYNCROOM Driver (WDM)」や"
            "「VoiceMeeter Input」を選びます"
        };

        for (int c = 0; c < 3; ++c)
            combos[c].setTooltip (jp (baseTips[c]) + suffix);
    }

    //==========================================================================
    void updateAdvice (const DeviceCatalog& catalog,
                       juce::ComboBox (&combos)[3],
                       juce::Label& hintLabel,
                       juce::Label& warningLabel)
    {
        const auto inputId   = selectedEndpointId (combos[0]);
        const auto monitorId = selectedEndpointId (combos[1]);
        const auto sendId    = selectedEndpointId (combos[2]);

        juce::String warning;
        juce::Colour warningColour = kDanger;

        // 1) 消えた機器の指摘が最優先。ここを飲み込むと「勝手に別の機器になった」に化ける。
        if (isMarkedMissing (combos[0]))
            warning = jp ("\xe2\x9a\xa0 前回のマイクが見つかりません。別の機器を選んでください。");
        else if (isMarkedMissing (combos[1]))
            warning = jp ("\xe2\x9a\xa0 前回の「自分に聞こえる音」の機器が見つかりません。別の機器を選んでください。");
        else if (isMarkedMissing (combos[2]))
            warning = jp ("\xe2\x9a\xa0 前回の「相手に送る音」の機器が見つかりません。"
                          "別の機器を選ぶか「送信しない」にしてください。");

        // 2) 構成そのものの検証。文言はカタログ側が日本語で持っている。
        if (warning.isEmpty())
        {
            const auto validation = catalog.validateRouting (inputId, monitorId, sendId);

            if (validation.problem != RoutingProblem::none && validation.japaneseMessage.isNotEmpty())
            {
                if (validation.blocksStart)
                {
                    warning = validation.japaneseMessage;
                }
                else if (validation.problem != RoutingProblem::sendNotSelected)
                {
                    warning = validation.japaneseMessage;
                    warningColour = kAmber;
                }
            }
        }

        if (warning.isNotEmpty())
        {
            warningLabel.setColour (juce::Label::textColourId, warningColour);
            warningLabel.setText (warning, juce::dontSendNotification);
            warningLabel.setVisible (true);
            hintLabel.setVisible (false);
            return;
        }

        warningLabel.setVisible (false);
        hintLabel.setVisible (true);

        if (DeviceCatalog::isCollapsible (inputId, monitorId))
        {
            // 推奨構成。ドリフト補正もリングも要らず、モニター遅延がデバイス遅延だけになる。
            hintLabel.setColour (juce::Label::textColourId, kAccent);
            hintLabel.setText (jp ("\xe2\x9c\x93 入力とモニターが同じ機器なので、いちばん遅れが少ない状態です。"),
                               juce::dontSendNotification);
        }
        else
        {
            hintLabel.setColour (juce::Label::textColourId, kDim);
            hintLabel.setText (jp ("「[低遅延]」と付いた機器を選ぶと遅れが少なくなります"),
                               juce::dontSendNotification);
        }
    }
}

//==============================================================================
DeviceSelectorPanel::DeviceSelectorPanel (DeviceCatalog& c)
    : catalog (c)
{
    sectionLabel.setText (jp ("\xe2\x91\xa0 機器を選ぶ"), juce::dontSendNotification);
    sectionLabel.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    sectionLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (sectionLabel);

    rescanButton.setButtonText (jp ("機器を再検索"));
    rescanButton.setTooltip (jp ("USB マイクを挿し直した後などに押してください"));
    rescanButton.onClick = [this] { listeners.call (&Listener::deviceRescanRequested); };
    addAndMakeVisible (rescanButton);

    static const char* const columnTitles[3] =
    {
        "入力（マイク）",
        "自分に聞こえる音（ヘッドホン）",
        "相手に送る音（SYNCROOM など）"
    };

    for (int i = 0; i < 3; ++i)
    {
        columnLabels[i].setText (jp (columnTitles[i]), juce::dontSendNotification);
        columnLabels[i].setFont (juce::Font (juce::FontOptions (13.0f)));
        columnLabels[i].setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (columnLabels[i]);

        combos[i].setTextWhenNothingSelected (jp (kNothingSelectedText));
        combos[i].setTextWhenNoChoicesAvailable (jp ("機器が見つかりません"));
        combos[i].setColour (juce::ComboBox::backgroundColourId, kField);
        combos[i].setColour (juce::ComboBox::textColourId, kText);
        combos[i].setColour (juce::ComboBox::outlineColourId, kPanel.brighter (0.20f));
        combos[i].setColour (juce::ComboBox::arrowColourId, kDim);
        // ヘッダに private メソッドを足さずに済ませるため、変更処理はここに直接書く。
        combos[i].onChange = [this, i]
        {
            auto& combo = combos[i];

            combo.getProperties().set (kDesiredIdKey, selectedEndpointId (combo));
            combo.setTextWhenNothingSelected (jp (kNothingSelectedText));

            applyAvailability (combos, catalog);
            updateAdvice (catalog, combos, hintLabel, warningLabel);

            listeners.call (&Listener::deviceSelectionChanged);
        };
        addAndMakeVisible (combos[i]);

        statusLines[i].setFont (juce::Font (juce::FontOptions (12.5f)));
        statusLines[i].setColour (juce::Label::textColourId, kDim);
        addAndMakeVisible (statusLines[i]);
    }

    hintLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    hintLabel.setColour (juce::Label::textColourId, kDim);
    hintLabel.setText (jp ("「[低遅延]」と付いた機器を選ぶと遅れが少なくなります"),
                       juce::dontSendNotification);
    addAndMakeVisible (hintLabel);

    warningLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    warningLabel.setColour (juce::Label::textColourId, kDanger);
    addChildComponent (warningLabel);

    catalog.addListener (this);
    refresh();
}

DeviceSelectorPanel::~DeviceSelectorPanel()
{
    catalog.removeListener (this);

    for (auto& combo : combos)
        combo.onChange = nullptr;
}

//==============================================================================
void DeviceSelectorPanel::setSelection (const juce::String& inputId,
                                        const juce::String& monitorId,
                                        const juce::String& sendId)
{
    combos[0].getProperties().set (kDesiredIdKey, inputId);
    combos[1].getProperties().set (kDesiredIdKey, monitorId);
    combos[2].getProperties().set (kDesiredIdKey, sendId);

    refresh();
}

juce::String DeviceSelectorPanel::getInputEndpointId() const   { return selectedEndpointId (combos[0]); }
juce::String DeviceSelectorPanel::getMonitorEndpointId() const { return selectedEndpointId (combos[1]); }
juce::String DeviceSelectorPanel::getSendEndpointId() const    { return selectedEndpointId (combos[2]); }

//==============================================================================
void DeviceSelectorPanel::setStatusLine (Column column, const juce::String& text, bool isError)
{
    const int i = juce::jlimit (0, 2, (int) column);

    statusLines[i].setColour (juce::Label::textColourId, isError ? kDanger : kDim);
    statusLines[i].setText (text, juce::dontSendNotification);
}

void DeviceSelectorPanel::setAdvancedBackendsVisible (bool shouldBeVisible)
{
    if (advancedVisible == shouldBeVisible)
        return;

    advancedVisible = shouldBeVisible;
    refresh();
}

void DeviceSelectorPanel::refresh()
{
    populateCombo (combos[0], catalog, Column::input,   advancedVisible);
    populateCombo (combos[1], catalog, Column::monitor, advancedVisible);
    populateCombo (combos[2], catalog, Column::send,    advancedVisible);

    applyDesiredSelection (combos[0], Column::input);
    applyDesiredSelection (combos[1], Column::monitor);
    applyDesiredSelection (combos[2], Column::send);

    applyAvailability (combos, catalog);
    updateAdvice (catalog, combos, hintLabel, warningLabel);
}

void DeviceSelectorPanel::deviceListChanged()
{
    // 使用中の機器が消えても勝手に別の機器へ切り替えない。選択を落として赤く出すだけ。
    refresh();
}

//==============================================================================
void DeviceSelectorPanel::addListener (Listener* l)    { listeners.add (l); }
void DeviceSelectorPanel::removeListener (Listener* l) { listeners.remove (l); }

//==============================================================================
void DeviceSelectorPanel::paint (juce::Graphics& g)
{
    g.setColour (kPanel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.0f);
}

void DeviceSelectorPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 6);

    auto header = area.removeFromTop (22);
    rescanButton.setBounds (header.removeFromRight (110).withSizeKeepingCentre (110, 22));
    sectionLabel.setBounds (header);

    area.removeFromTop (2);

    auto advice = area.removeFromBottom (15);
    hintLabel.setBounds (advice);
    warningLabel.setBounds (advice);

    constexpr int gap = 12;
    const int columnWidth = juce::jmax (80, (area.getWidth() - 2 * gap) / 3);

    // 高さは余りから決める。パネルの実寸が 116 px からずれても崩れないようにする。
    const int labelH  = 15;
    const int statusH = 13;
    const int comboH  = juce::jlimit (24, 36, area.getHeight() - labelH - statusH - 3);

    for (int i = 0; i < 3; ++i)
    {
        auto column = area.removeFromLeft (i == 2 ? area.getWidth() : columnWidth);

        if (i < 2)
            area.removeFromLeft (gap);

        columnLabels[i].setBounds (column.removeFromTop (labelH));
        combos[i].setBounds (column.removeFromTop (comboH));
        column.removeFromTop (2);
        statusLines[i].setBounds (column.removeFromTop (statusH));
    }
}

} // namespace kvc
