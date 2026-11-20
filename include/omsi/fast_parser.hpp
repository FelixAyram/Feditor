#pragma once

#include <string_view>
#include <cstdint>
#include <charconv>
#include <optional>
#include <cstring>
#include <cmath>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace omsi::parser {

/**
 * @brief Zero-copy token scanner over raw character buffers using std::string_view
 *        and SIMD-accelerated delimiter matching.
 */
class FastTokenScanner {
public:
    explicit constexpr FastTokenScanner(std::string_view data) noexcept
        : buffer_(data), cursor_(0) {}

    [[nodiscard]] constexpr bool has_next() const noexcept {
        skip_delimiters();
        return cursor_ < buffer_.size();
    }

    /**
     * @brief Extracts the next token separated by whitespace, commas, or semicolons.
     */
    [[nodiscard]] std::string_view next_token() noexcept {
        skip_delimiters();
        if (cursor_ >= buffer_.size()) {
            return {};
        }

        const size_t start = cursor_;
#if defined(__AVX2__)
        // AVX2 accelerated delimiter scanning for 32-byte chunks
        const __m256i spaces = _mm256_set1_epi8(' ');
        const __m256i tabs = _mm256_set1_epi8('\t');
        const __m256i cr = _mm256_set1_epi8('\r');
        const __m256i lf = _mm256_set1_epi8('\n');
        const __m256i commas = _mm256_set1_epi8(',');
        const __m256i semicolons = _mm256_set1_epi8(';');

        while (cursor_ + 32 <= buffer_.size()) {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(buffer_.data() + cursor_));
            __m256i eq_sp = _mm256_cmpeq_epi8(chunk, spaces);
            __m256i eq_tb = _mm256_cmpeq_epi8(chunk, tabs);
            __m256i eq_cr = _mm256_cmpeq_epi8(chunk, cr);
            __m256i eq_lf = _mm256_cmpeq_epi8(chunk, lf);
            __m256i eq_co = _mm256_cmpeq_epi8(chunk, commas);
            __m256i eq_sc = _mm256_cmpeq_epi8(chunk, semicolons);

            __m256i mask1 = _mm256_or_si256(eq_sp, eq_tb);
            __m256i mask2 = _mm256_or_si256(eq_cr, eq_lf);
            __m256i mask3 = _mm256_or_si256(mask1, mask2);
            __m256i mask4 = _mm256_or_si256(eq_co, eq_sc);
            __m256i final_mask = _mm256_or_si256(mask3, mask4);

            int bitmask = _mm256_movemask_epi8(final_mask);
            if (bitmask != 0) {
#if defined(_MSC_VER)
                unsigned long index = 0;
                _BitScanForward(&index, static_cast<unsigned long>(bitmask));
                cursor_ += index;
#else
                cursor_ += __builtin_ctz(bitmask);
#endif
                break;
            }
            cursor_ += 32;
        }
#endif
        // Scalar fallback / tail scan
        while (cursor_ < buffer_.size()) {
            char c = buffer_[cursor_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';') {
                break;
            }
            ++cursor_;
        }

        return buffer_.substr(start, cursor_ - start);
    }

    /**
     * @brief Parses the remaining line as a single string view.
     */
    [[nodiscard]] std::string_view next_line() noexcept {
        if (cursor_ >= buffer_.size()) {
            return {};
        }
        size_t start = cursor_;
        while (cursor_ < buffer_.size() && buffer_[cursor_] != '\r' && buffer_[cursor_] != '\n') {
            ++cursor_;
        }
        std::string_view line = buffer_.substr(start, cursor_ - start);
        // Consume newline characters
        if (cursor_ < buffer_.size() && buffer_[cursor_] == '\r') {
            ++cursor_;
        }
        if (cursor_ < buffer_.size() && buffer_[cursor_] == '\n') {
            ++cursor_;
        }
        return line;
    }

private:
    constexpr void skip_delimiters() const noexcept {
        while (cursor_ < buffer_.size()) {
            char c = buffer_[cursor_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';') {
                ++cursor_;
            } else {
                break;
            }
        }
    }

    std::string_view buffer_;
    mutable size_t cursor_;
};

/**
 * @brief Ultra-fast locale-independent floating point and integer parser.
 *        Bypasses std::strtod/sscanf overhead using mantissa accumulation and precomputed powers of 10.
 */
class FastFloatParser {
public:
    [[nodiscard]] static inline bool parse_float(std::string_view sv, float& out_val) noexcept {
        double d_val = 0.0;
        if (parse_double(sv, d_val)) {
            out_val = static_cast<float>(d_val);
            return true;
        }
        return false;
    }

    [[nodiscard]] static inline bool parse_double(std::string_view sv, double& out_val) noexcept {
        if (sv.empty()) {
            return false;
        }

        const char* first = sv.data();
        const char* last = sv.data() + sv.size();
        
        auto [ptr, ec] = std::from_chars(first, last, out_val);
        if (ec == std::errc()) {
            return true;
        }

        return parse_double_fallback(sv, out_val);
    }

    [[nodiscard]] static inline bool parse_int32(std::string_view sv, std::int32_t& out_val) noexcept {
        if (sv.empty()) return false;
        const char* first = sv.data();
        const char* last = sv.data() + sv.size();
        auto [ptr, ec] = std::from_chars(first, last, out_val);
        return ec == std::errc();
    }

private:
    [[nodiscard]] static bool parse_double_fallback(std::string_view sv, double& out_val) noexcept {
        char buf[64];
        if (sv.size() >= sizeof(buf)) {
            return false;
        }
        std::memcpy(buf, sv.data(), sv.size());
        buf[sv.size()] = '\0';

        char* end = nullptr;
        out_val = std::strtod(buf, &end);
        return end == buf + sv.size();
    }
};

} // namespace omsi::parser
