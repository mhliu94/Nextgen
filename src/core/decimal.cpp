#include "core/decimal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ng {
namespace {

using int128 = Decimal::int128;
using uint128 = unsigned __int128;

constexpr int kMaxScale = 60;      // generous ceiling; real inputs use <= 24 decimals
constexpr int kMaxPow10 = 38;      // 10^38 fits in a signed 128-bit integer

const std::array<int128, kMaxPow10 + 1>& pow10_table() {
    static const std::array<int128, kMaxPow10 + 1> table = [] {
        std::array<int128, kMaxPow10 + 1> t{};
        t[0] = 1;
        for (int i = 1; i <= kMaxPow10; ++i) t[i] = t[i - 1] * 10;
        return t;
    }();
    return table;
}

int128 pow10(int n) {
    if (n < 0 || n > kMaxPow10) throw DecimalError("decimal exponent out of range");
    return pow10_table()[static_cast<std::size_t>(n)];
}

bool mul_overflow(int128 a, int128 b, int128* out) { return __builtin_mul_overflow(a, b, out); }
bool add_overflow(int128 a, int128 b, int128* out) { return __builtin_add_overflow(a, b, out); }

int count_digits(int128 value) {
    if (value < 0) value = -value;
    int digits = 0;
    uint128 v = static_cast<uint128>(value);
    while (v != 0) {
        v /= 10;
        ++digits;
    }
    return digits;
}

std::string uint128_to_string(uint128 value) {
    if (value == 0) return "0";
    char buf[64];
    int pos = 63;
    buf[pos] = '\0';
    while (value != 0) {
        buf[--pos] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    }
    return std::string(buf + pos);
}

// Floor division for signed 128-bit integers.
int128 floor_div_int(int128 a, int128 b) {
    int128 q = a / b;
    int128 r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

int128 ceil_div_int(int128 a, int128 b) {
    int128 q = a / b;
    int128 r = a % b;
    if (r != 0 && ((r < 0) == (b < 0))) ++q;
    return q;
}

}  // namespace

Decimal::Decimal(int128 mantissa, int scale) : mant_(mantissa), scale_(scale) {
    if (scale < 0) {
        int128 m;
        if (mul_overflow(mantissa, pow10(-scale), &m)) throw DecimalError("decimal overflow");
        mant_ = m;
        scale_ = 0;
    } else if (scale > kMaxScale) {
        throw DecimalError("decimal scale out of range");
    }
}

const Decimal& Decimal::zero() {
    static const Decimal z;
    return z;
}

const Decimal& Decimal::one() {
    static const Decimal o(1);
    return o;
}

Decimal Decimal::parse(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    if (begin >= end) throw DecimalError("empty decimal");
    std::string_view s = text.substr(begin, end - begin);

    bool negative = false;
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') {
        negative = s[i] == '-';
        ++i;
    }
    uint128 mant = 0;
    int digits_seen = 0, frac_digits = 0, significant = 0;
    bool in_fraction = false, any_digit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            any_digit = true;
            ++digits_seen;
            if (in_fraction) ++frac_digits;
            if (mant != 0 || c != '0') ++significant;
            if (significant > kMaxPow10) throw DecimalError("decimal exceeds supported precision");
            mant = mant * 10 + static_cast<unsigned>(c - '0');
        } else if (c == '.' && !in_fraction) {
            in_fraction = true;
        } else if (c == '_' && any_digit) {
            continue;  // Python accepts underscores between digits
        } else {
            break;
        }
    }
    if (!any_digit) throw DecimalError("invalid decimal");
    int exponent = 0;
    if (i < s.size()) {
        if (s[i] != 'e' && s[i] != 'E') throw DecimalError("invalid decimal");
        ++i;
        bool exp_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exp_negative = s[i] == '-';
            ++i;
        }
        if (i >= s.size()) throw DecimalError("invalid decimal exponent");
        long exp_value = 0;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') throw DecimalError("invalid decimal exponent");
            exp_value = exp_value * 10 + (s[i] - '0');
            if (exp_value > 10000) throw DecimalError("decimal exponent out of range");
        }
        exponent = static_cast<int>(exp_negative ? -exp_value : exp_value);
    }
    int scale = frac_digits - exponent;
    int128 signed_mant = static_cast<int128>(mant);
    if (negative) signed_mant = -signed_mant;
    if (mant == 0) {
        return Decimal(0, std::clamp(scale, 0, kMaxScale), Raw{});
    }
    if (scale < 0) {
        if (-scale > kMaxPow10) throw DecimalError("decimal exceeds supported precision");
        int128 m;
        if (mul_overflow(signed_mant, pow10(-scale), &m)) throw DecimalError("decimal exceeds supported precision");
        return Decimal(m, 0, Raw{});
    }
    if (scale > kMaxScale) throw DecimalError("decimal exceeds supported precision");
    return Decimal(signed_mant, scale, Raw{});
}

std::optional<Decimal> Decimal::try_parse(std::string_view text) noexcept {
    try {
        return parse(text);
    } catch (const DecimalError&) {
        return std::nullopt;
    }
}

Decimal Decimal::from_double(double value) {
    if (!std::isfinite(value)) throw DecimalError("non-finite double");
    char buf[64];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);  // shortest round-trip
    if (result.ec != std::errc()) throw DecimalError("double conversion failed");
    return parse(std::string_view(buf, static_cast<std::size_t>(result.ptr - buf)));
}

bool Decimal::is_integral() const noexcept {
    if (scale_ == 0) return true;
    return mant_ % pow10_table()[static_cast<std::size_t>(std::min(scale_, kMaxPow10))] == 0 && scale_ <= kMaxPow10;
}

int Decimal::digit_count() const noexcept { return count_digits(mant_); }

int Decimal::adjusted() const noexcept {
    if (mant_ == 0) return -scale_;
    return count_digits(mant_) - 1 - scale_;
}

std::string Decimal::str() const {
    uint128 magnitude = mant_ < 0 ? static_cast<uint128>(-mant_) : static_cast<uint128>(mant_);
    std::string digits = uint128_to_string(magnitude);
    std::string out;
    if (mant_ < 0) out.push_back('-');
    if (scale_ == 0) {
        out += digits;
        return out;
    }
    if (static_cast<int>(digits.size()) <= scale_) {
        digits.insert(0, static_cast<std::size_t>(scale_ + 1 - static_cast<int>(digits.size())), '0');
    }
    std::size_t split = digits.size() - static_cast<std::size_t>(scale_);
    out.append(digits, 0, split);
    out.push_back('.');
    out.append(digits, split, std::string::npos);
    return out;
}

std::string Decimal::normalized_str() const { return normalized().str(); }

Decimal Decimal::normalized() const noexcept {
    int128 m = mant_;
    int s = scale_;
    if (m == 0) return Decimal(0, 0, Raw{});
    while (s > 0 && m % 10 == 0) {
        m /= 10;
        --s;
    }
    return Decimal(m, s, Raw{});
}

double Decimal::to_double() const {
    // Parse the plain string to get a correctly rounded double.
    return std::strtod(str().c_str(), nullptr);
}

long long Decimal::to_int64() const {
    if (!is_integral()) throw DecimalError("decimal is not integral");
    int128 whole = scale_ == 0 ? mant_ : mant_ / pow10(scale_);
    if (whole > static_cast<int128>(INT64_MAX) || whole < static_cast<int128>(INT64_MIN)) {
        throw DecimalError("decimal out of int64 range");
    }
    return static_cast<long long>(whole);
}

void Decimal::align(Decimal& a, Decimal& b) {
    if (a.scale_ == b.scale_) return;
    Decimal* lo = a.scale_ < b.scale_ ? &a : &b;
    Decimal* hi = a.scale_ < b.scale_ ? &b : &a;
    while (lo->scale_ < hi->scale_) {
        int diff = hi->scale_ - lo->scale_;
        int step = std::min(diff, kMaxPow10);
        int128 m;
        if (!mul_overflow(lo->mant_, pow10(step), &m)) {
            lo->mant_ = m;
            lo->scale_ += step;
            continue;
        }
        // Raising the low-scale value would overflow: shed digits from the high-scale one.
        if (hi->scale_ == 0) throw DecimalError("decimal overflow");
        hi->mant_ /= 10;
        hi->scale_ -= 1;
    }
}

Decimal Decimal::operator+(const Decimal& other) const {
    Decimal a = *this, b = other;
    align(a, b);
    int128 m;
    if (add_overflow(a.mant_, b.mant_, &m)) throw DecimalError("decimal overflow");
    return Decimal(m, a.scale_, Raw{});
}

Decimal Decimal::operator-(const Decimal& other) const { return *this + (-other); }

Decimal Decimal::operator*(const Decimal& other) const {
    Decimal a = *this, b = other;
    int128 m;
    while (mul_overflow(a.mant_, b.mant_, &m)) {
        // Shed least-significant fractional digits from the operand with the larger scale.
        Decimal* victim = a.scale_ >= b.scale_ ? &a : &b;
        if (victim->scale_ == 0) throw DecimalError("decimal overflow");
        victim->mant_ /= 10;
        victim->scale_ -= 1;
    }
    int scale = a.scale_ + b.scale_;
    Decimal result(m, 0, Raw{});
    result.scale_ = scale;
    if (scale > kMaxScale) {
        // Reduce precision to the supported ceiling.
        while (result.scale_ > kMaxScale) {
            result.mant_ /= 10;
            result.scale_ -= 1;
        }
    }
    return result;
}

std::strong_ordering Decimal::operator<=>(const Decimal& other) const {
    if (mant_ == 0 && other.mant_ == 0) return std::strong_ordering::equal;
    if ((mant_ < 0) != (other.mant_ < 0)) {
        return mant_ < other.mant_ ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    Decimal a = *this, b = other;
    try {
        align(a, b);
    } catch (const DecimalError&) {
        // Extreme magnitudes: compare adjusted exponents then leading digits.
        int ea = adjusted(), eb = other.adjusted();
        if (ea != eb) return (ea < eb) == (mant_ > 0) ? std::strong_ordering::less : std::strong_ordering::greater;
        return str() < other.str() ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (a.mant_ < b.mant_) return std::strong_ordering::less;
    if (a.mant_ > b.mant_) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

Decimal Decimal::scaleb(int n) const {
    if (n == 0) return *this;
    if (n < 0) {
        int scale = scale_ - n;
        if (scale > kMaxScale) throw DecimalError("decimal scale out of range");
        return Decimal(mant_, scale, Raw{});
    }
    Decimal result = *this;
    while (n > 0) {
        if (result.scale_ > 0) {
            result.scale_ -= 1;
        } else {
            int128 m;
            if (mul_overflow(result.mant_, 10, &m)) throw DecimalError("decimal overflow");
            result.mant_ = m;
        }
        --n;
    }
    return result;
}

Decimal::int128 Decimal::floor_div(const Decimal& divisor) const {
    if (divisor.mant_ == 0) throw DecimalError("division by zero");
    Decimal a = *this, b = divisor;
    align(a, b);
    return floor_div_int(a.mant_, b.mant_);
}

Decimal::int128 Decimal::ceil_div(const Decimal& divisor) const {
    if (divisor.mant_ == 0) throw DecimalError("division by zero");
    Decimal a = *this, b = divisor;
    align(a, b);
    return ceil_div_int(a.mant_, b.mant_);
}

Decimal Decimal::round_down_to_step(const Decimal& step) const {
    int128 q = floor_div(step);
    return Decimal(q, 0, Raw{}) * step;
}

Decimal Decimal::round_up_to_step(const Decimal& step) const {
    int128 q = ceil_div(step);
    return Decimal(q, 0, Raw{}) * step;
}

Decimal Decimal::div(const Decimal& divisor, int fractional_digits) const {
    if (divisor.mant_ == 0) throw DecimalError("division by zero");
    if (fractional_digits < 0) fractional_digits = 0;
    if (fractional_digits > kMaxPow10) fractional_digits = kMaxPow10;
    // a / b = (am / bm) * 10^(bs - as). Strip trailing zeros first so large mantissas with many
    // fractional digits (e.g. 2.000...0 at scale 30) keep their headroom for the quotient.
    int128 am = mant_;
    int as = scale_;
    int128 bm = divisor.mant_;
    int bs = divisor.scale_;
    while (am != 0 && am % 10 == 0) {
        am /= 10;
        --as;
    }
    while (bm % 10 == 0) {
        bm /= 10;
        --bs;
    }
    // Result mantissa at `digits` decimals: floor(am * 10^(digits + bs - as) / bm), truncated toward zero.
    for (int digits = fractional_digits;; --digits) {
        int k = digits + bs - as;
        int128 num = am;
        int128 den = bm;
        bool fits = true;
        if (k >= 0) {
            fits = k <= kMaxPow10 && !mul_overflow(am, pow10(k), &num);
        } else {
            // |a / b| < 10^-digits once the divisor needs more than 10^38 of scaling: the truncated quotient is 0.
            if (-k > kMaxPow10) return Decimal(0, digits, Raw{});
            fits = !mul_overflow(bm, pow10(-k), &den);
        }
        if (fits) return Decimal(num / den, digits, Raw{});
        if (digits == 0) throw DecimalError("decimal overflow");
    }
}

std::string to_string(const Decimal& value) { return value.str(); }

}  // namespace ng
