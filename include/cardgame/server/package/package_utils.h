#pragma once

#include <boost/json/value.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace cardgame::server::package {

// RFC 8785 JSON Canonicalization Scheme。该函数用于跨平台 manifest_id，
// 不能替换成普通 JSON pretty printer 或只排序对象键的实现。
[[nodiscard]] std::string canonicalize_json(const boost::json::value& value);

[[nodiscard]] std::string sha256(std::string_view bytes);
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

[[nodiscard]] bool is_lower_hex_hash(std::string_view text, std::size_t size);
[[nodiscard]] bool is_valid_utf8(std::string_view text, std::size_t* code_points = nullptr);

}  // namespace cardgame::server::package
