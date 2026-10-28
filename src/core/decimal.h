// Exact decimal arithmetic for order quantities, prices and participation credit.
//
// Values are stored as a 128-bit integer mantissa with a non-negative decimal
// scale (value = mantissa / 10^scale). Addition, subtraction, multiplication and
// comparison are exact; division is provided only as explicit floor/ceil step
// rounding (exact) or as a truncated quotient for diagnostics. Inputs beyond
// ~38 significant digits are rejected rather than silently rounded.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ng {

class DecimalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Decimal {
public:
    using int128 = __int128;

    constexpr Decimal() noexcept : mant_(0), scale_(0) {}
    explicit Decimal(long long value) noexcept : mant_(value), scale_(0) {}
    Decimal(int128 mantissa, int scale);

    // Parses [+-]digits[.digits][(e|E)[+-]digits]. Rejects NaN/Infinity, blanks and junk.
    static Decimal parse(std::string_view text);
    static std::optional<Decimal> try_parse(std::string_view text) noexcept;
    // Shortest round-trip representation of a double, like Python's str(float).
    static Decimal from_double(double value);

    static const Decimal& zero();
    static const Decimal& one();

    int128 mantissa() const noexcept { return mant_; }
    int scale() const noexcept { return scale_; }
    bool is_zero() const noexcept { return mant_ == 0; }
    bool is_negative() const noexcept { return mant_ < 0; }
    bool is_positive() const noexcept { return mant_ > 0; }
    int sign() const noexcept { return mant_ < 0 ? -1 : (mant_ > 0 ? 1 : 0); }
    bool is_integral() const noexcept;
    // Number of significant digits of the mantissa (0 for zero).
    int digit_count() const noexcept;
    // Exponent of the most significant digit, like Python's Decimal.adjusted().
    int adjusted() const noexcept;
    // Exponent of the least significant digit (= -scale).
    int exponent() const noexcept { return -scale_; }

    // Plain notation with exactly `scale` fractional digits (Python format(d, "f")).
    std::string str() const;
    // Plain notation without trailing fractional zeros.
    std::string normalized_str() const;
    double to_double() const;
    // Throws unless the value is integral and fits.
    long long to_int64() const;

    Decimal operator-() const noexcept { return Decimal(-mant_, scale_, Raw{}); }
    Decimal operator+(const Decimal& other) const;
    Decimal operator-(const Decimal& other) const;
    Decimal operator*(const Decimal& other) const;
    Decimal& operator+=(const Decimal& other) { return *this = *this + other; }
    Decimal& operator-=(const Decimal& other) { return *this = *this - other; }
    Decimal& operator*=(const Decimal& other) { return *this = *this * other; }

    std::strong_ordering operator<=>(const Decimal& other) const;
    bool operator==(const Decimal& other) const { return (*this <=> other) == 0; }

    // Multiply by 10^n exactly.
    Decimal scaleb(int n) const;
    // floor(this / step) * step and ceil(this / step) * step, exact.
    Decimal round_down_to_step(const Decimal& step) const;
    Decimal round_up_to_step(const Decimal& step) const;
    // Exact integer floor division (this / divisor), divisor != 0.
    int128 floor_div(const Decimal& divisor) const;
    int128 ceil_div(const Decimal& divisor) const;
    // Quotient truncated toward zero to `fractional_digits` decimals (diagnostics only).
    Decimal div(const Decimal& divisor, int fractional_digits = 18) const;
    // Drop trailing fractional zeros from the representation.
    Decimal normalized() const noexcept;
    Decimal abs() const noexcept { return mant_ < 0 ? -*this : *this; }

    static Decimal min(const Decimal& a, const Decimal& b) { return a <= b ? a : b; }
    static Decimal max(const Decimal& a, const Decimal& b) { return a >= b ? a : b; }

private:
    struct Raw {};
    constexpr Decimal(int128 mantissa, int scale, Raw) noexcept : mant_(mantissa), scale_(scale) {}
    // Align two values to a common scale; may shed least-significant digits
    // when a value would otherwise overflow 128 bits.
    static void align(Decimal& a, Decimal& b);

    int128 mant_;
    int scale_;
};

std::string to_string(const Decimal& value);

}  // namespace ng
