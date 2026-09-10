#include "mine_teleop/detail/console_assets.hpp"

#include <fstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace mine_teleop {
ServerResponse serve_console_asset(const DriverConfig& config, std::string_view path) {
  static const std::unordered_map<std::string, std::string> types{
      {".js", "text/javascript; charset=utf-8"}, {".css", "text/css; charset=utf-8"},
      {".json", "application/json"}, {".glb", "model/gltf-binary"},
      {".gltf", "model/gltf+json"}, {".fbx", "application/octet-stream"},
      {".obj", "text/plain"}, {".mtl", "text/plain"}, {".bin", "application/octet-stream"},
      {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
      {".webp", "image/webp"}};
  const auto missing = [] { return ServerResponse::json(404, {{"error", "visual asset not found"}}); };
  if (config.visual_assets_root.empty() || !path.starts_with("/assets/")) return missing();
  const std::string name(path.substr(8));
  if (name.empty() || name.find_first_of("\\:") != std::string::npos ||
      name.find('\0') != std::string::npos) return missing();
  const std::filesystem::path relative(name);
  if (relative.is_absolute()) return missing();
  for (const auto& part : relative) if (part == ".." || part == ".") return missing();
  auto extension = relative.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  const auto type = types.find(extension);
  if (type == types.end()) return missing();
  std::error_code error;
  const auto root = std::filesystem::canonical(config.visual_assets_root, error);
  if (error) return missing();
  const auto file = std::filesystem::canonical(root / relative, error);
  if (error) return missing();
  const auto contained = file.lexically_relative(root);
  if (contained.empty() || contained.is_absolute()) return missing();
  for (const auto& part : contained) if (part == "..") return missing();
  if (!std::filesystem::is_regular_file(file, error) || error) return missing();
  const auto size = std::filesystem::file_size(file, error);
  if (error || size > 128 * 1024 * 1024) return missing();
  std::ifstream input(file, std::ios::binary);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(size))) return missing();
  auto response = ServerResponse::text(200, std::move(bytes), type->second);
  response.headers.emplace_back("X-Content-Type-Options", "nosniff");
  response.headers.emplace_back("Cache-Control", "no-cache");
  return response;
}
}
