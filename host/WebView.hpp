#pragma once
#include "ui/Application.hpp"
#include "ui/Runtime.hpp"
namespace foil::host {
// Serves the running app to a browser: docked 1920x1080 frames as PNG, and
// keys, taps and drags back into the same Application the console runs.
int serveWeb(ui::Application &app, ui::Runtime &runtime, int port);
} // namespace foil::host
