#pragma once

#include <charconv>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>
#ifdef __EMSCRIPTEN__
#include <limits>
#include <type_traits>
#endif

namespace devilution {

#ifdef __EMSCRIPTEN__
/** Decimal from_chars equivalent for the browser simulation core.
 * Avoid libc++'s optimized charconv implementation, which has produced incorrect
 * narrow and 32-bit results in optimized WASM. Scan every digit on overflow and
 * leave the destination unchanged on any error, as std::from_chars requires. */
template <typename T>
std::from_chars_result ParseBrowserDecimal(const char *begin, const char *end, T &destination)
{
	static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
	const char *cursor = begin;
	bool negative = false;
	if constexpr (std::is_signed_v<T>) {
		if (cursor != end && *cursor == '-') {
			negative = true;
			++cursor;
		}
	}
	const char *digits = cursor;
	uint64_t limit = static_cast<uint64_t>(std::numeric_limits<T>::max());
	if constexpr (std::is_signed_v<T>) {
		if (negative)
			limit = static_cast<uint64_t>(-(std::numeric_limits<T>::min() + int64_t { 1 })) + 1;
	}
	const uint64_t cutoff = limit / 10;
	const unsigned lastDigit = static_cast<unsigned>(limit % 10);
	uint64_t value = 0;
	bool overflow = false;
	while (cursor != end) {
		const unsigned digit = static_cast<unsigned char>(*cursor) - static_cast<unsigned>('0');
		if (digit > 9)
			break;
		if (!overflow) {
			if (value > cutoff || (value == cutoff && digit > lastDigit))
				overflow = true;
			else
				value = value * 10 + digit;
		}
		++cursor;
	}
	if (cursor == digits)
		return { begin, std::errc::invalid_argument };
	if (overflow)
		return { cursor, std::errc::result_out_of_range };
	if constexpr (std::is_signed_v<T>) {
		if (negative) {
			destination = value == limit ? std::numeric_limits<T>::min() : static_cast<T>(-static_cast<int64_t>(value));
			return { cursor, {} };
		}
	}
	destination = static_cast<T>(value);
	return { cursor, {} };
}
#endif

/** Decimal-only charconv entry point shared by tables, INI and imported manifests. */
template <typename T>
std::from_chars_result ParseDecimal(const char *begin, const char *end, T &destination)
{
#ifdef __EMSCRIPTEN__
	return ParseBrowserDecimal(begin, end, destination);
#else
	return std::from_chars(begin, end, destination);
#endif
}

enum class ParseIntError {
	ParseError = 1,
	OutOfRange
};

template <typename IntT>
using ParseIntResult = std::expected<IntT, ParseIntError>;

template <typename IntT>
ParseIntResult<IntT> ParseInt(
    std::string_view str, IntT min = std::numeric_limits<IntT>::min(),
    IntT max = std::numeric_limits<IntT>::max(), const char **endOfParse = nullptr)
{
	IntT value;
	const std::from_chars_result result = ParseDecimal(str.data(), str.data() + str.size(), value);
	if (endOfParse != nullptr) {
		*endOfParse = result.ptr;
	}
	if (result.ec == std::errc::invalid_argument)
		return std::unexpected(ParseIntError::ParseError);
	if (result.ec == std::errc::result_out_of_range || value < min || value > max)
		return std::unexpected(ParseIntError::OutOfRange);
	if (result.ec != std::errc())
		return std::unexpected(ParseIntError::ParseError);
	return value;
}

/**
 * @brief Parses a sequence of decimal characters into a 6 bit fixed point number in the range [0, 1.0]
 * @param str a potentially empty string of base 10 digits, optionally followed by non-digit characters
 * @param[out] endOfParse equivalent to std::from_chars_result::ptr, used to tell where parsing stopped
 * @return a value in the range [0, 64], representing a 2.6 fixed value in the range [0, 1.0]
 */
uint8_t ParseFixed6Fraction(std::string_view str, const char **endOfParse = nullptr);

template <typename IntT>
ParseIntResult<IntT> ParseFixed6(std::string_view str, const char **endOfParse = nullptr)
{
	if (endOfParse != nullptr) {
		// To allow for early returns we set the end pointer to the start of the string, which is the common case for errors.
		*endOfParse = str.data();
	}

	if (str.empty()) {
		return std::unexpected { ParseIntError::ParseError };
	}

	constexpr IntT minIntegerValue = std::numeric_limits<IntT>::min() >> 6;
	constexpr IntT maxIntegerValue = std::numeric_limits<IntT>::max() >> 6;

	const char *currentChar; // will be set by the call to parseInt
	ParseIntResult<IntT> integerParseResult = ParseInt(str, minIntegerValue, maxIntegerValue, &currentChar);

	bool isNegative = std::is_signed_v<IntT> && str[0] == '-';
	bool haveDigits = integerParseResult.has_value() || integerParseResult.error() == ParseIntError::OutOfRange;
	if (haveDigits) {
		str.remove_prefix(static_cast<size_t>(std::distance(str.data(), currentChar)));
	} else if (isNegative) {
		str.remove_prefix(1);
	}

	// if the string has no leading digits we still need to try parse the fraction part
	uint8_t fractionPart = 0;
	if (!str.empty() && str[0] == '.') {
		// got a fractional part to read too
		str.remove_prefix(1); // skip past the decimal point

		fractionPart = ParseFixed6Fraction(str, &currentChar);
		haveDigits = haveDigits || str.data() != currentChar;
	}

	if (!haveDigits) {
		// early return in case we got a string like "-.abc", don't want to set the end pointer in this case
		return std::unexpected { ParseIntError::ParseError };
	}

	if (endOfParse != nullptr) {
		*endOfParse = currentChar;
	}

	if (!integerParseResult.has_value() && integerParseResult.error() == ParseIntError::OutOfRange) {
		// if the integer parsing gave us an out of range value then we've done a bit of unnecessary
		//  work parsing the fraction part, but it saves duplicating code.
		return integerParseResult;
	}
	// integerParseResult could be a ParseError at this point because of a string like ".123" or "-.1"
	//  so we need to default to 0 (and use the result of the minus sign check when it's relevant)
	IntT integerPart = integerParseResult.value_or(0);

	// rounding could give us a value of 64 for the fraction part (e.g. 0.993 rounds to 1.0) so we need to ensure this doesn't overflow
	if (fractionPart >= 64 && (integerPart >= maxIntegerValue || (std::is_signed_v<IntT> && integerPart <= minIntegerValue))) {
		return std::unexpected { ParseIntError::OutOfRange };
	} else {
		IntT fixedValue = integerPart << 6;
		if (isNegative) {
			fixedValue -= fractionPart;
		} else {
			fixedValue += fractionPart;
		}
		return fixedValue;
	}
}

} // namespace devilution
