#pragma once

#include "desktop.h"
#include "animations/animation_manager.h"
#include "views/desktop_view_pin_timeout.h"
#include "views/desktop_view_pin_input.h"
#include "views/desktop_view_locked.h"
#include "views/desktop_view_main.h"
#include "views/desktop_view_first_start.h"
#include "views/desktop_view_lock_menu.h"
#include "views/desktop_view_debug.h"
#include "desktop/desktop_settings/desktop_settings.h"

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_stack.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/popup.h>
#include <gui/scene_manager.h>

#include <loader/loader.h>

#define STATUS_BAR_Y_SHIFT 13

typedef enum {
    DesktopViewIdMain,
    DesktopViewIdLockMenu,
    DesktopViewIdLocked,
    DesktopViewIdDebug,
    DesktopViewIdFirstStart,
    DesktopViewIdHwMismatch,
    DesktopViewIdPinInput,
    DesktopViewIdPinTimeout,
    DesktopViewIdTotal,
} DesktopViewId;

struct Desktop {
    // Scene
    FuriThread* scene_thread;
    // GUI
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    DesktopFirstStartView* first_start_view;
    Popup* hw_mismatch_popup;
    DesktopLockMenuView* lock_menu;
    DesktopDebugView* debug_view;
    DesktopViewLocked* locked_view;
    DesktopMainView* main_view;
    DesktopViewPinTimeout* pin_timeout_view;

    ViewStack* main_view_stack;
    ViewStack* locked_view_stack;

    DesktopSettings settings;
    DesktopViewPinInput* pin_input_view;

    ViewPort* lock_viewport;

    AnimationManager* animation_manager;
    Loader* loader;
    FuriPubSubSubscription* app_start_stop_subscription;
};

Desktop* desktop_alloc();

void desktop_free(Desktop* desktop);
