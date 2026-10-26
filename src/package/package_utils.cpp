#include "cardgame/server/package/package_utils.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>

#include <array>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace cardgame::server::package {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

class Sha256 final {
public:
    void update(std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform();
                bit_count_ += 512;
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() {
        bit_count_ += static_cast<std::uint64_t>(buffer_size_) * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            while (buffer_size_ < 64) {
                buffer_[buffer_size_++] = 0;
            }
            transform();
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56) {
            buffer_[buffer_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffer_size_++] =
                static_cast<std::uint8_t>((bit_count_ >> shift) & 0xffU);
        }
        transform();

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24U);
            digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16U);
            digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8U);
            digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    void transform() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            words[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24U) |
                           (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16U) |
                           (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8U) |
                           static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t s0 = std::rotr(words[index - 15], 7) ^
                                     std::rotr(words[index - 15], 18) ^
                                     (words[index - 15] >> 3U);
            const std::uint32_t s1 = std::rotr(words[index - 2], 17) ^
                                     std::rotr(words[index - 2], 19) ^
                                     (words[index - 2] >> 10U);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                                       std::rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choice +
                                        round_constants[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                                       std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

std::string hex_digest(const std::array<std::uint8_t, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const std::uint8_t byte : digest) {
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

std::vector<std::uint16_t> utf16_units(std::string_view text) {
    std::vector<std::uint16_t> result;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    for (std::size_t index = 0; index < text.size();) {
        std::uint32_t point = bytes[index];
        std::size_t count = 1;
        if (point >= 0xf0U) {
            point &= 0x07U;
            count = 4;
        } else if (point >= 0xe0U) {
            point &= 0x0fU;
            count = 3;
        } else if (point >= 0xc0U) {
            point &= 0x1fU;
            count = 2;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            point = (point << 6U) | (bytes[index + offset] & 0x3fU);
        }
        if (point <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(point));
        } else {
            point -= 0x10000U;
            result.push_back(static_cast<std::uint16_t>(0xd800U + (point >> 10U)));
            result.push_back(static_cast<std::uint16_t>(0xdc00U + (point & 0x3ffU)));
        }
        index += count;
    }
    return result;
}

void append_string(std::string& output, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : text) {
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\t': output += "\\t"; break;
        case '\n': output += "\\n"; break;
        case '\f': output += "\\f"; break;
        case '\r': output += "\\r"; break;
        default:
            if (byte < 0x20U) {
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
        }
    }
    output.push_back('"');
}

void append_number(std::string& output, double number) {
    if (!std::isfinite(number)) {
        throw std::invalid_argument{"RFC 8785 rejects non-finite numbers"};
    }
    if (number == 0.0) {
        output.push_back('0');
        return;
    }
    std::array<char, 64> buffer{};
    std::string text;
    // libstdc++ 的无 precision to_chars 在部分值上会留下非最短尾数。
    // 从最少有效位开始寻找能精确 round-trip 的表示，得到 RFC 8785 所需
    // 的 ECMAScript 最短十进制表示。
    for (int precision = 1; precision <= std::numeric_limits<double>::max_digits10;
         ++precision) {
        const auto [end, error] = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), number,
            std::chars_format::general, precision);
        if (error != std::errc{}) {
            throw std::runtime_error{"failed to canonicalize JSON number"};
        }
        const std::string candidate(buffer.data(), end);
        std::istringstream parser(candidate);
        parser.imbue(std::locale::classic());
        double round_trip = 0;
        parser >> round_trip;
        if (parser && parser.peek() == std::char_traits<char>::eof() &&
            std::bit_cast<std::uint64_t>(round_trip) ==
                std::bit_cast<std::uint64_t>(number)) {
            text = candidate;
            break;
        }
    }
    if (text.empty()) {
        throw std::runtime_error{"failed to find round-trip JSON number"};
    }
    const std::size_t exponent = text.find('e');
    if (exponent != std::string::npos) {
        const double absolute = std::abs(number);
        const int exponent_value = std::stoi(text.substr(exponent + 1));
        if (absolute >= 1e-6 && absolute < 1e21) {
            const bool negative = text.front() == '-';
            const std::size_t mantissa_begin = negative ? 1 : 0;
            const std::string mantissa = text.substr(
                mantissa_begin, exponent - mantissa_begin);
            const std::size_t dot = mantissa.find('.');
            std::string digits = mantissa;
            if (dot != std::string::npos) {
                digits.erase(dot, 1);
            }
            const int before_decimal = static_cast<int>(
                dot == std::string::npos ? mantissa.size() : dot);
            const int decimal_position = before_decimal + exponent_value;
            std::string fixed;
            if (negative) fixed.push_back('-');
            if (decimal_position <= 0) {
                fixed += "0.";
                fixed.append(static_cast<std::size_t>(-decimal_position), '0');
                fixed += digits;
            } else if (decimal_position >= static_cast<int>(digits.size())) {
                fixed += digits;
                fixed.append(
                    static_cast<std::size_t>(decimal_position) - digits.size(), '0');
            } else {
                fixed.append(digits, 0, static_cast<std::size_t>(decimal_position));
                fixed.push_back('.');
                fixed.append(digits, static_cast<std::size_t>(decimal_position));
            }
            text = std::move(fixed);
        } else {
            const std::size_t sign = exponent + 1;
            std::size_t digits = sign;
            if (text[digits] == '+' || text[digits] == '-') {
                ++digits;
            }
            while (digits + 1 < text.size() && text[digits] == '0') {
                text.erase(digits, 1);
            }
            if (text[sign] != '-' && text[sign] != '+') {
                text.insert(sign, 1, '+');
            }
        }
    }
    output += text;
}

void append_canonical(std::string& output, const boost::json::value& value) {
    if (value.is_null()) {
        output += "null";
    } else if (value.is_bool()) {
        output += value.as_bool() ? "true" : "false";
    } else if (value.is_int64()) {
        output += std::to_string(value.as_int64());
    } else if (value.is_uint64()) {
        output += std::to_string(value.as_uint64());
    } else if (value.is_double()) {
        append_number(output, value.as_double());
    } else if (value.is_string()) {
        append_string(output, value.as_string());
    } else if (value.is_array()) {
        output.push_back('[');
        bool first = true;
        for (const auto& item : value.as_array()) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            append_canonical(output, item);
        }
        output.push_back(']');
    } else {
        struct Member {
            std::string_view key;
            const boost::json::value* value;
            std::vector<std::uint16_t> order;
        };
        std::vector<Member> members;
        members.reserve(value.as_object().size());
        for (const auto& member : value.as_object()) {
            members.push_back({member.key(), &member.value(), utf16_units(member.key())});
        }
        std::sort(members.begin(), members.end(), [](const Member& left, const Member& right) {
            return left.order < right.order;
        });
        output.push_back('{');
        bool first = true;
        for (const Member& member : members) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            append_string(output, member.key);
            output.push_back(':');
            append_canonical(output, *member.value);
        }
        output.push_back('}');
    }
}

}  // namespace

std::string canonicalize_json(const boost::json::value& value) {
    std::string output;
    append_canonical(output, value);
    return output;
}

std::string sha256(std::string_view bytes) {
    Sha256 hasher;
    hasher.update(bytes);
    return hex_digest(hasher.finish());
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error{"cannot read file for SHA-256: " + path.string()};
    }
    Sha256 hasher;
    std::array<char, 16384> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hasher.update(std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
    }
    if (!input.eof()) {
        throw std::runtime_error{"failed while reading file for SHA-256: " + path.string()};
    }
    return hex_digest(hasher.finish());
}

bool is_lower_hex_hash(std::string_view text, std::size_t size) {
    if (text.size() != size) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

bool is_valid_utf8(std::string_view text, std::size_t* code_points) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    std::size_t count = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index];
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7fU) {
            ++index;
            ++count;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1;
        ++count;
    }
    if (code_points != nullptr) {
        *code_points = count;
    }
    return true;
}

}  // namespace cardgame::server::package
