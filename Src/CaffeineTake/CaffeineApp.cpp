// CaffeineTake - Keep your computer awake.
// 
// Copyright (c) 2020-2021 VacuityBox
// Copyright (c) 2022      serverfailure71
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// 
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CaffeineApp.hpp"

#include "Dialogs/AboutDialog.hpp"
#include "Dialogs/CaffeineSettings.hpp"
#include "Resource.hpp"
#include "Utility.hpp"
#include "Version.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <commctrl.h>
#include <Psapi.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <WtsApi32.h>

using namespace std;

namespace CaffeineTake {

CaffeineApp::CaffeineApp(const AppInitInfo& info)
    : mSettings           (std::make_shared<Settings>())
    , mSettingsFilePath   (info.SettingsPath)
    , mCustomIconsPath    (info.DataDirectory / "Icons" / "")
    , mInstanceHandle     (info.InstanceHandle)
    , mInitialized        (false)
    , mSessionState       (SessionState::Unlocked)
    , mNotifyIcon         (mni::NotifyIcon::Desc{
            .instance    = info.InstanceHandle,
            .windowTitle = L"CaffeineTray_InvisibleWindow",
            .className   = L"CaffeineTray_WndClass"
        }
    )
    , mThemeInfo           (mni::ThemeInfo::Detect())
    , mIcons               (info.InstanceHandle)
    , mCaffeineState       (CaffeineState::Inactive)
    , mCaffeineMode        (CaffeineMode::Disabled)
    , mKeepDisplayOn       (false)
    , mAppSO               (this)
    , mAutoMode            (&mAppSO, mSettings)
    , mTimerMode           (&mAppSO, mSettings)
{
    spdlog::info("---- Log started ----");
}

CaffeineApp::~CaffeineApp()
{
    DisableCaffeine();

    spdlog::info("---- Log ended ----");
}

auto CaffeineApp::Init() -> bool
{
    spdlog::info("Initializing CaffeineTake...");

    // Load Settings.
    {
        // Create default settings file if not exists.
        if (!fs::exists(mSettingsFilePath))
        {
            spdlog::warn("Settings file not found, creating default one");
            SaveSettings();
        }
        else
        {
            if (!LoadSettings())
            {
                return false;
            }
        }

        mThemeInfo = mni::ThemeInfo::Detect();
        mSessionState = IsSessionLocked();

        spdlog::info("System theme: {}", static_cast<int>(mThemeInfo.GetTheme()));
        spdlog::info("Session state: {}", mSessionState == SessionState::Unlocked ? "Unlocked" : "Locked");
    }

    // For hyperlinks in About dialog.
    {
        auto ccs   = INITCOMMONCONTROLSEX{ 0 };
        ccs.dwSize = sizeof(ccs);
        ccs.dwICC  = ICC_LINK_CLASS;
        InitCommonControlsEx(&ccs);
    }

    // Create NotifyIcon.
    {
        if (FAILED(mNotifyIcon.Init()))
        {
            spdlog::error("Failed to create NotifyIcon");
            return false;
        }

        spdlog::info("Created NotifyIcon");

        // Register callbacks.
        mNotifyIcon.OnCreate            = std::bind(&CaffeineApp::OnCreate, this);
        mNotifyIcon.OnDestroy           = std::bind(&CaffeineApp::OnDestroy, this);
        mNotifyIcon.OnLmbClick          = std::bind(&CaffeineApp::OnClick, this, std::placeholders::_1, std::placeholders::_2);
        mNotifyIcon.OnContextMenuOpen   = std::bind(&CaffeineApp::OnContextMenuOpen, this);
        mNotifyIcon.OnContextMenuSelect = std::bind(&CaffeineApp::OnContextMenuSelect, this, std::placeholders::_1);
        mNotifyIcon.OnThemeChange       = std::bind(&CaffeineApp::OnThemeChange, this, std::placeholders::_1);
        mNotifyIcon.OnDpiChange         = std::bind(&CaffeineApp::OnDpiChange, this, std::placeholders::_1);
        mNotifyIcon.OnCustomMessage     = std::bind(&CaffeineApp::OnCustomMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        mNotifyIcon.OnSystemMessage     = std::bind(&CaffeineApp::OnSystemMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

        mNotifyIcon.Show();
    }

    // Load icons.
    {
        // TODO get dpi and load proper size
        mIcons.Load(mSettings->IconPack, mThemeInfo.IsDark() ? CaffeineIcons::Theme::Light : CaffeineIcons::Theme::Dark, 16, 16);
    }

    // Update icons, timer, power settings.
    {
        SetCaffeineMode(mCaffeineMode);
        UpdateAppIcon();
    }

    mInitialized = true;
    spdlog::info("Initialization finished");

    return true;
}

auto CaffeineApp::MainLoop () -> int
{
    return mNotifyIcon.MainLoop();
}

auto CaffeineApp::OnCreate() -> void
{
    // Add session lock notification event.
    if (!WTSRegisterSessionNotification(mNotifyIcon.Handle(), NOTIFY_FOR_THIS_SESSION))
    {
        spdlog::error("Failed to register session notification event");
        spdlog::info("DisableOnLockScreen functionality will not work");
    }
}

auto CaffeineApp::OnDestroy() -> void
{
    spdlog::info("Shutting down application");
    WTSUnRegisterSessionNotification(mNotifyIcon.Handle());
}


auto CaffeineApp::OnClick(int x, int y) -> void
{
    spdlog::trace("NotifyIcon::OnClick");
    ToggleCaffeineMode();
}

auto CaffeineApp::OnContextMenuOpen() -> void
{
    spdlog::trace("NotifyIcon::OnContextMenuOpen");

    auto hMenu = HMENU{0};
    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        hMenu = LoadMenuW(mInstanceHandle, MAKEINTRESOURCE(IDC_CAFFEINE_DISABLED_CONTEXTMENU));
        break;
    case CaffeineMode::Enabled:
        hMenu = LoadMenuW(mInstanceHandle, MAKEINTRESOURCE(IDC_CAFFEINE_ENABLED_CONTEXTMENU));
        break;
    case CaffeineMode::Auto:
        hMenu = LoadMenuW(mInstanceHandle, MAKEINTRESOURCE(IDC_CAFFEINE_AUTO_CONTEXTMENU));
        break;
    // TODO change when timer menu added
    case CaffeineMode::Timer:
        hMenu = LoadMenuW(mInstanceHandle, MAKEINTRESOURCE(IDC_CAFFEINE_AUTO_CONTEXTMENU));
        break;
    }

    mNotifyIcon.SetMenu(hMenu);
}

auto CaffeineApp::OnContextMenuSelect(int selectedItem) -> void
{
    spdlog::trace("NotifyIcon::OnContextMenuSelect(selectedItem={})", selectedItem);

    switch (selectedItem)
    {
    // TODO is this used?
    case IDM_TOGGLE_CAFFEINE:
        ToggleCaffeineMode();
        return;

    case IDM_DISABLE_CAFFEINE:
        SetCaffeineMode(CaffeineMode::Disabled);
        return;

    case IDM_ENABLE_CAFFEINE:
        SetCaffeineMode(CaffeineMode::Enabled);
        return;

    case IDM_ENABLE_AUTO:
        SetCaffeineMode(CaffeineMode::Auto);
        return;

    case IDM_SETTINGS:
        ShowSettingsDialog();
        return;

    case IDM_ABOUT:
        ShowAboutDialog();
        return;

    case IDM_EXIT:
        mNotifyIcon.Quit();
        return;
    }
}

auto CaffeineApp::OnThemeChange(mni::ThemeInfo ti) -> void
{
    spdlog::info("System theme changed, new theme: {}", static_cast<int>(ti.GetTheme()));

    mThemeInfo = ti;

    // Load proper icons.
    // TODO save dpi
    // TODO pick right icons for high contrast
    mIcons.Load(mSettings->IconPack, mThemeInfo.IsDark() ? CaffeineIcons::Theme::Light : CaffeineIcons::Theme::Dark, 16, 16);

    UpdateIcon();
    UpdateAppIcon();
}

auto CaffeineApp::OnDpiChange(int dpi) -> void
{
    spdlog::info("System dpi changed, new dpi: {}", dpi);

    const auto w = (16 * dpi) / 96;
    const auto h = (16 * dpi) / 96;

    // TODO save dpi
    // TODO pick right icons for high contrast
    // it can be specific icon override
    mIcons.Load(mSettings->IconPack, mThemeInfo.IsDark() ? CaffeineIcons::Theme::Light : CaffeineIcons::Theme::Dark, w, h);

    UpdateIcon();
}

auto CaffeineApp::OnCustomMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) -> void
{
    spdlog::trace("NotifyIcon::OnCustomMessage(uMsg={})", uMsg);
    switch (uMsg)
    {
    case WM_CAFFEINE_TAKE_UPDATE_EXECUTION_STATE:
        if (static_cast<bool>(wParam))
        {
            UpdateExecutionState(CaffeineState::Active);
        }
        else
        {
            UpdateExecutionState(CaffeineState::Inactive);
        }
        break;
    }
}

auto CaffeineApp::OnSystemMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) -> bool
{
    spdlog::trace("NotifyIcon::OnSystemMessage(uMsg={})", uMsg);
    switch (uMsg)
    {
    case WM_WTSSESSION_CHANGE:
        switch (wParam)
        {
        case WTS_SESSION_LOCK:
            spdlog::info("Session lock event");
            mSessionState = SessionState::Locked;
            RefreshExecutionState();
            return true;

        case WTS_SESSION_UNLOCK:
            spdlog::info("Session unlock event");
            mSessionState = SessionState::Unlocked;
            RefreshExecutionState();
            return true;
        }

        break;
    }

    return false;
}

auto CaffeineApp::EnableCaffeine () -> bool
{
    spdlog::trace("EnableCaffeine()");
    mNotifyIcon.SendCustomMessage(WM_CAFFEINE_TAKE_UPDATE_EXECUTION_STATE, static_cast<WPARAM>(true), NULL);
    return true;
}

auto CaffeineApp::DisableCaffeine () -> bool
{
    spdlog::trace("DisableCaffeine()");
    mNotifyIcon.SendCustomMessage(WM_CAFFEINE_TAKE_UPDATE_EXECUTION_STATE, static_cast<WPARAM>(false), NULL);
    return true;
}

auto CaffeineApp::ToggleCaffeineMode() -> void
{
    spdlog::trace("ToggleCaffeineMode()");
    
    auto mode = CaffeineMode::Disabled;
    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        mode = CaffeineMode::Enabled;
        break;
    case CaffeineMode::Enabled:
        mode = CaffeineMode::Auto;
        break;
    case CaffeineMode::Auto:
        mode = CaffeineMode::Timer;
        break;
    case CaffeineMode::Timer:
        mode = CaffeineMode::Disabled;
        break;
    }

    SetCaffeineMode(mode);
}

auto CaffeineApp::SetCaffeineMode(CaffeineMode mode) -> void
{
    spdlog::info(L"Setting CaffeineMode to {}", CaffeineModeToString(mode));

    // Stop current mode.
    StopMode();

    // Start new one.
    mCaffeineMode = mode;
    StartMode();

    UpdateIcon();
    UpdateTip();
}

auto CaffeineApp::StartMode () -> void
{
    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        mDisabledMode.Start(&mAppSO);
        return;

    case CaffeineMode::Enabled:
        mEnabledMode.Start(&mAppSO);
        break;

    case CaffeineMode::Auto:
        mAutoMode.Start(&mAppSO);
        break;

    case CaffeineMode::Timer:
        mTimerMode.Start(&mAppSO);
        break;
    }
}

auto CaffeineApp::StopMode () -> void
{
    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        mDisabledMode.Stop(&mAppSO);
        return;

    case CaffeineMode::Enabled:
        mEnabledMode.Stop(&mAppSO);
        break;

    case CaffeineMode::Auto:
        mAutoMode.Stop(&mAppSO);
        break;

    case CaffeineMode::Timer:
        mTimerMode.Stop(&mAppSO);
        break;
    }
}

auto CaffeineApp::UpdateExecutionState(CaffeineState state) -> void
{
    auto keepDisplayOn = false;
    auto disableOnLock = false;

    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        break;

    case CaffeineMode::Enabled:
        keepDisplayOn = mSettings->Standard.KeepDisplayOn;
        disableOnLock = mSettings->Standard.DisableOnLockScreen;
        break;

    case CaffeineMode::Auto:
        keepDisplayOn = mSettings->Auto.KeepDisplayOn;
        disableOnLock = mSettings->Auto.DisableOnLockScreen;
        break;

    case CaffeineMode::Timer:
        keepDisplayOn = mSettings->Timer.KeepDisplayOn;
        disableOnLock = mSettings->Timer.DisableOnLockScreen;
        break;
    }

    if (mCaffeineMode != CaffeineMode::Disabled)
    {
        if (mSessionState == SessionState::Locked && keepDisplayOn)
        {
            keepDisplayOn = !disableOnLock;
        }

        if (mCaffeineState == state && mKeepDisplayOn == keepDisplayOn)
        {
            spdlog::debug("No need to update execution state, continuing");
            return;
        }
    }
    else
    {
        if (mCaffeineState == state)
        {
            spdlog::debug("No need to update execution state, continuing");
            return;
        }
    }

    // Update Execution State.
    mCaffeineState = state;
    mKeepDisplayOn = keepDisplayOn;

    auto flags = EXECUTION_STATE{ES_CONTINUOUS};
    if (mCaffeineState == CaffeineState::Active)
    {
        flags |= ES_SYSTEM_REQUIRED;

        if (keepDisplayOn)
        {
            flags |= ES_DISPLAY_REQUIRED;
        }
    }

    if (!SetThreadExecutionState(flags))
    {
        spdlog::error("Failed to update execution state");
        return;
    }

    spdlog::info("Updated execution state");

    UpdateIcon();
    UpdateTip();
}

auto CaffeineApp::RefreshExecutionState () -> void
{
    UpdateExecutionState(mCaffeineState);
}

auto CaffeineApp::UpdateIcon() -> bool
{
    auto icon = HICON{0};

    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        icon = mIcons.CaffeineDisabled;
        break;
    case CaffeineMode::Enabled:
        icon = mIcons.CaffeineEnabled;
        break;
    case CaffeineMode::Auto:
        if (mCaffeineState == CaffeineState::Inactive)
        {
            icon = mIcons.CaffeineAutoInactive;
        }
        else
        {
            icon = mIcons.CaffeineAutoActive;
        }
        break;
    case CaffeineMode::Timer:
        if (mCaffeineState == CaffeineState::Inactive)
        {
            icon = mIcons.CaffeineTimerInactive;
        }
        else
        {
            icon = mIcons.CaffeineTimerActive;
        }
        break;
    }

    // No need to update.
    if (mNotifyIcon.GetIcon() == icon)
    {
        return false;
    }

    if (FAILED(mNotifyIcon.SetIcon(icon)))
    {
        spdlog::error("Failed to update notifyicon icon");
        return false;
    }

    spdlog::info("Updated notifyicon icon");
    return true;
}

auto CaffeineApp::UpdateTip() -> bool
{
    auto tip  = UINT{0};

    switch (mCaffeineMode)
    {
    case CaffeineMode::Disabled:
        tip = IDS_CAFFEINE_DISABLED;
        break;
    case CaffeineMode::Enabled:
        tip = IDS_CAFFEINE_ENABLED;
        break;
    case CaffeineMode::Auto:
        if (mCaffeineState == CaffeineState::Inactive)
        {
            tip = IDS_CAFFEINE_AUTO_INACTIVE;
        }
        else
        {
            tip = IDS_CAFFEINE_AUTO_ACTIVE;
        }
        break;
    case CaffeineMode::Timer:
        if (mCaffeineState == CaffeineState::Inactive)
        {
            tip = IDS_CAFFEINE_TIMER_INACTIVE;
        }
        else
        {
            tip = IDS_CAFFEINE_TIMER_ACTIVE;
        }
        break;
    }

    auto buffer = std::array<wchar_t, 128>();
    buffer.fill(L'\0');
    
    LoadStringW(mInstanceHandle, tip, buffer.data(), buffer.size());

    // No need to update.
    if (mNotifyIcon.GetTip() == buffer.data())
    {
        return false;
    }

    if (FAILED(mNotifyIcon.SetTip(buffer.data())))
    {
        spdlog::error("Failed to update notifyicon tip");
        return false;
    }

    spdlog::info("Updated notifyicon tip");
    return true;
}

auto CaffeineApp::UpdateAppIcon() -> void
{
    // TODO sometimes icon in taskmanger is invalid
    // TODO is this function needed, maybe making a proper icon to look good on both themes, white icon with black outline or something
    auto icon = [&](){
        // TODO dpi load?
        const auto flags = UINT{ LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED };
        if (mThemeInfo.IsLight())
        {
            return static_cast<HICON>(
                LoadImageW(mInstanceHandle, MAKEINTRESOURCEW(IDI_CAFFEINE_APP_DARK), IMAGE_ICON, 0, 0, flags)
            );
        }
        else
        {
            return static_cast<HICON>(
                LoadImageW(mInstanceHandle, MAKEINTRESOURCEW(IDI_CAFFEINE_APP_LIGHT), IMAGE_ICON, 0, 0, flags)
            );
        }
    }();


    SendMessage(mNotifyIcon.Handle(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    SendMessage(mNotifyIcon.Handle(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
}

auto CaffeineApp::LoadSettings() -> bool
{
    // NOTE: Settings should be in UTF-8
    // Read Settings file.
    auto file = std::ifstream(mSettingsFilePath);
    if (!file)
    {
        spdlog::error("Can't open Settings file '" + mSettingsFilePath.string() + "' for reading");
        return false;
    }

    // Deserialize.
    auto json = nlohmann::json::parse(file, nullptr, false, true);
    if (json.is_discarded())
    {
        spdlog::error("Failed to deserialize json");
        return false;
    }
    
    try
    {
        *mSettings = json.get<Settings>();
    }
    catch (nlohmann::json::exception&)
    {
        spdlog::error("Failed to deserialize settings");
        spdlog::warn("Using default values");
        *mSettings = Settings();
        return true;
    }

    spdlog::debug("{}", json.dump(4));
    spdlog::info("Loaded Settings '" + mSettingsFilePath.string() + "'");

    return true;
}

auto CaffeineApp::SaveSettings() -> bool
{
    // Open Settings file.
    auto file = std::ofstream(mSettingsFilePath);
    if (!file)
    {
        spdlog::error("Can't open Settings file '" + mSettingsFilePath.string() + "' for writing");
        return false;
    }

    // Serialize.
    auto json = nlohmann::json(*mSettings);
    file << std::setw(4) << json;

    spdlog::info("Saved Settings '" + mSettingsFilePath.string() + "'");
    return true;
}

auto CaffeineApp::ShowSettingsDialog () -> bool
{
    SINGLE_INSTANCE_GUARD();
    
    auto caffeineSettings = CaffeineSettings(mSettings);
    if (caffeineSettings.Show(mNotifyIcon.Handle()))
    {
        const auto& newSettings = caffeineSettings.Result();

        mSettings->Standard = newSettings.Standard;
        mSettings->Auto     = newSettings.Auto;

        // TODO in future settings change might change auto mode refresh interval, so update timer settings        

        // Settings change don't trigger caffeine state to change,
        // but display settings might change so we need to update.
        RefreshExecutionState();

        SaveSettings();
    }

    return true;
}

auto CaffeineApp::ShowAboutDialog () -> bool
{
    SINGLE_INSTANCE_GUARD();
    
    auto aboutDlg = AboutDialog();
    aboutDlg.Show(mNotifyIcon.Handle());

    return true;
}

} // namespace CaffeineTake