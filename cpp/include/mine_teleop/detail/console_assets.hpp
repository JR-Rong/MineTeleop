#pragma once

#include "mine_teleop/server.hpp"

namespace mine_teleop {
ServerResponse serve_console_asset(const DriverConfig& config, std::string_view path);
}
