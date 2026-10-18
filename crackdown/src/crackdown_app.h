// crackdown - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/filesystem.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/window.h>

#include "ap/bridge.h"
#include "host_tuning.h"
#include "input_replay.h"
#include "pc_options.h"
#include "pc_settings.h"
#include "xui_patch.h"

class CrackdownApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<CrackdownApp>(new CrackdownApp(ctx, "crackdown",
        PPCImageConfig));
  }

  // Earliest hook after cvars are parsed and logging is up.
  void OnPostInitLogging() override {
    crackdown::StartProfiler();
    crackdown::OpenPerfCsvLog();
    crackdown::ApplyGpuPluginDefault();
  }

  // No-argument starts: default game folder and GPU plugin.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    crackdown::ApplyStartupDefaults(paths);
  }

  // After the GPU plugin has loaded (its flags only exist from then on) and
  // right before the guest starts.
  void OnPreLaunchModule() override {
    crackdown::ApplyGameDefaults();
    crackdown::ApplySimRate(runtime()->memory());
    crackdown::ApplyFpsCap(runtime()->memory());
    crackdown::DumpGuestImage(runtime());
    SetGuestFrameStats(&crackdown::GuestFrameStats);
    // Capture effective cvars after GPU flags/defaults exist; edits are pending
    // until restart (including fullscreen). Use the launcher's adjacent file.
    crackdown::PCSettings().Initialize(rex::filesystem::GetExecutableFolder() / "crackdown.toml",
        rex::cvar::Query<bool>("vsync"), rex::cvar::Query<int32_t>("resolution_scale"),
        rex::cvar::Query<bool>("fullscreen"));
    crackdown::SetPCMenuQuitAction(
        [this] {
          return app_context().CallInUIThreadDeferred([this] {
            REXLOG_INFO("pc menu: closing window for Quit to Desktop");
            crackdown::FlushPerfCsvLog();
            if (window()) window()->RequestClose();
          });
        });
    crackdown::PreparePCMenu(runtime(), game_data_root(), cache_root());
    crackdown::ap::Initialize(user_data_root());
    crackdown::SetInputReplayQuit([this] {
      app_context().CallInUIThreadDeferred([this] {
        crackdown::FlushPerfCsvLog();
        if (window()) window()->RequestClose();
      });
    });
  }

  // Close button / Alt+F4. The SDK then hard-exits, so flush the perf CSV now.
  bool OnWindowCloseRequested() override {
    crackdown::FlushPerfCsvLog();
    crackdown::ap::Shutdown();
    return true;
  }

  // F11 switches between fullscreen (borderless) and windowed while playing.
  // Not saved: crackdown.toml / the launcher still pick the startup mode.
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle fullscreen", [this] {
      if (window()) window()->SetFullscreen(!window()->IsFullscreen());
    });
  }

  // Override virtual hooks for customization:
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
