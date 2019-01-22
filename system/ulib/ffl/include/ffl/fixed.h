// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#pragma once

//
// Fuchsia Fixed-point Library (FFL):
//
// An efficient header-only multi-precision fixed point math library with well-defined rounding.
//

#include <type_traits>

#include <ffl/expression.h>
#include <ffl/fixed_format.h>
#include <ffl/utility.h>

namespace ffl {

template <typename Integer, size_t FractionalBits>
class Fixed {
public:
    using Format = FixedFormat<Integer, FractionalBits>;

    constexpr Fixed() = default;
    constexpr Fixed(const Fixed&) = default;
    constexpr Fixed& operator=(const Fixed&) = default;

    template <Operation Op, typename... Args>
    constexpr Fixed(Expression<Op, Args...> expression) {
        *this = expression;
    }
    template <Operation Op, typename... Args>
    constexpr Fixed& operator=(Expression<Op, Args...> expression) {
        const auto value = Format::Convert(expression.Evaluate(Format{}));
        value_ = Format::Saturate(value.value);
        return *this;
    }

    constexpr Fixed(Value<Format> value) {
        *this = value;
    }
    constexpr Fixed& operator=(Value<Format> value) {
        value_ = Format::Saturate(value.value);
        return *this;
    }

    constexpr Integer raw_value() const { return value_; }
    constexpr Value<Format> value() const { return Value<Format>{value_}; }

    constexpr Integer Ceiling() const {
        const auto value = Format::ToIntermediate(value_);
        return Format::Saturate(value + Format::FractionalMask) / Format::Power;
    }
    constexpr Integer Floor() const { return value_ / Format::Power; }
    constexpr Integer Round() const {
        return Format::Saturate(Format::Round(value_)) / Format::Power;
    }

    // Relational operators for same-typed values.
    constexpr bool operator<(Fixed other) const { return value_ < other.value_; }
    constexpr bool operator>(Fixed other) const { return value_ > other.value_; }
    constexpr bool operator<=(Fixed other) const { return value_ <= other.value_; }
    constexpr bool operator>=(Fixed other) const { return value_ >= other.value_; }
    constexpr bool operator==(Fixed other) const { return value_ == other.value_; }
    constexpr bool operator!=(Fixed other) const { return value_ != other.value_; }

    // Compound assignment operators.
    template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
    constexpr Fixed& operator+=(T expression) {
        *this = *this + expression;
        return *this;
    }
    template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
    constexpr Fixed& operator-=(T expression) {
        *this = *this - expression;
        return *this;
    }
    template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
    constexpr Fixed& operator*=(T expression) {
        *this = *this * expression;
        return *this;
    }
    template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
    constexpr Fixed& operator/=(T expression) {
        *this = *this / expression;
        return *this;
    }

private:
    Integer value_;
};

// Utility to round an expression to the given Integer.
template <typename Integer, typename T, typename Enabled = EnableIfUnaryExpression<T>>
inline constexpr auto Round(T expression) {
    const Fixed<Integer, 0> value{ToExpression<T>{expression}};
    return value.Round();
}

// Utility to create an Expression node from an integer value. May be used to
// initialize a Fixed variable from an integer.
template <typename Integer, typename Enabled = std::enable_if_t<std::is_integral_v<Integer>>>
inline constexpr auto FromInteger(Integer value) {
    return ToExpression<Integer>{value};
}

// Utility to create an Expression node from an integer ratio. May be used to
// initialize a Fixed variable from a ratio.
template <typename Integer, typename Enabled = std::enable_if_t<std::is_integral_v<Integer>>>
inline constexpr auto FromRatio(Integer numerator, Integer denominator) {
    return DivisionExpression<Integer, Integer>{numerator, denominator};
}

// Utility to coerce the an expression to the given precision.
template <size_t FractionalBits, typename T>
inline constexpr auto ToPrecision(T expression) {
    return PrecisionExpression<FractionalBits, T>{Init{}, expression};
}

// Utility to create a value Expression from a raw integer value already in the
// fixed-point format with the given number of fractional bits.
template <size_t FractionalBits, typename Integer>
inline constexpr auto FromRaw(Integer value) {
    return ValueExpression<Integer, FractionalBits>{value};
}

template <typename Integer, size_t FractionalBits>
inline constexpr auto Max(Fixed<Integer, FractionalBits> a, Fixed<Integer, FractionalBits> b) {
    return a > b ? a : b;
}

template <typename Integer, size_t FractionalBits>
inline constexpr auto Min(Fixed<Integer, FractionalBits> a, Fixed<Integer, FractionalBits> b) {
    return a < b ? a : b;
}

// Relational operators.
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator<(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) < Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator>(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) > Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator<=(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) <= Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator>=(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) >= Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator==(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) == Traits::Right(right);
}
template <typename Left, typename Right,
          typename Enabled = EnableIfComparisonExpression<Left, Right>>
inline constexpr bool operator!=(Left left, Right right) {
    using Traits = ComparisonTraits<Left, Right>;
    return Traits::Left(left) != Traits::Right(right);
}

// Arithmetic operators.
template <typename Left, typename Right,
          typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator+(Left left, Right right) {
    return AdditionExpression<Left, Right>{left, right};
}
template <typename T, typename Enabled = EnableIfUnaryExpression<T>>
inline constexpr auto operator-(T value) {
    return NegationExpression<T>{Init{}, value};
}
template <typename Left, typename Right,
          typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator-(Left left, Right right) {
    return SubtractionExpression<Left, Right>{left, right};
}
template <typename Left, typename Right,
          typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator*(Left left, Right right) {
    return MultiplicationExpression<Left, Right>{left, right};
}
template <typename Left, typename Right,
          typename Enabled = EnableIfBinaryExpression<Left, Right>>
inline constexpr auto operator/(Left left, Right right) {
    return DivisionExpression<Left, Right>{left, right};
}

} // namespace ffl
