// Asset-free regression for optimized charconv on the browser game's fiber.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <emscripten/fiber.h>

#include "data/iterators.hpp"

namespace {
emscripten_fiber_t HostFiber {}, GameFiber {};
std::vector<unsigned char> GameStack(64U * 1024U * 1024U);
std::vector<unsigned char> GameAsyncStack(4U * 1024U * 1024U);
std::vector<unsigned char> HostAsyncStack(4U * 1024U * 1024U);
unsigned Failures = 0;
unsigned Checks = 0;

struct Monster {
	unsigned char type;
	std::string name;
	std::string translation;
	unsigned char level;
	uint16_t hp;
};

template <typename T>
void Check(std::string text, T expected, bool success)
{
	text += "\tNextColumn\t99999\r\n";
	devilution::GetFieldResult state { text.data() };
	devilution::DataFileField field(&state, text.data() + text.size(), 1, 0);
	T destination = static_cast<T>(17);
	const auto result = field.parseInt(destination);
	++Checks;
	if (result.has_value() != success
	    || (success && destination != expected)
	    || (!success && destination != static_cast<T>(17))) {
		++Failures;
		std::fprintf(stderr, "field parser failed: %s (%zu-byte destination)\n", text.c_str(), sizeof(T));
	}
}

void Run(void *)
{
	// Keep the original failing shape: a narrow member in a vector element,
	// with the rest of the TSV buffer available to charconv. No logging or
	// scalar parse before this operation (either masked the optimizer defect).
	std::string text = "120\tGharbad\t3\t8\t16\t0\t0";
	for (int i = 0; i < 1000; ++i)
		text += "\nhello\t456\t99999\t99999";
	std::vector<Monster> monsters;
	monsters.reserve(150);
	Monster &monster = monsters.emplace_back();
	monster.level = 4;
	devilution::GetFieldResult state { text.data() };
	devilution::DataFileField field(&state, text.data() + text.size(), 1, 4);
	const auto result = field.parseInt(monster.hp);
	++Checks;
	if (!result.has_value() || monster.hp != 120) {
		++Failures;
		std::fprintf(stderr, "optimized fiber maxHp120 parsed as %u (success=%d)\n", monster.hp, result.has_value());
	}
	Check<uint16_t>("65535", 65535, true);
	Check<uint16_t>("65536", 0, false);
	Check<uint16_t>("-1", 0, false);
	Check<uint16_t>("+120", 0, false);
	Check<uint16_t>("abc", 0, false);
	Check<uint16_t>("120suffix", 120, true); // Retain existing numeric-prefix acceptance.
	Check<int16_t>("-32768", -32768, true);
	Check<int16_t>("-32769", 0, false);
	Check<int16_t>("32767", 32767, true);
	Check<int16_t>("32768", 0, false);
	Check<uint8_t>("255", 255, true);
	Check<uint8_t>("256", 0, false);
	Check<int8_t>("-128", -128, true);
	Check<int8_t>("-129", 0, false);
	Check<uint16_t>("999999999999999999999999999999", 0, false);
	Check<uint32_t>("1583495809", 1583495809U, true);
	Check<uint32_t>("4294967295", UINT32_MAX, true);
	Check<uint32_t>("4294967296", 0, false);
	Check<int64_t>("9223372036854775807", INT64_MAX, true);
	Check<int64_t>("9223372036854775808", 0, false);
	Check<int64_t>("-9223372036854775808", INT64_MIN, true);
	Check<int64_t>("-9223372036854775809", 0, false);
	Check<uint64_t>("18446744073709551615", UINT64_MAX, true);
	Check<uint64_t>("18446744073709551616", 0, false);
	// The general utility shares the browser parser, including caller bounds.
	const auto experience = devilution::ParseInt<uint32_t>("1583495809");
	++Checks;
	if (!experience.has_value() || experience.value() != 1583495809U) ++Failures;
	const auto bounded = devilution::ParseInt<int16_t>("-12", -10, 10);
	++Checks;
	if (bounded.has_value() || bounded.error() != devilution::ParseIntError::OutOfRange) ++Failures;

	emscripten_fiber_swap(&GameFiber, &HostFiber);
	std::abort();
}
} // namespace

int main()
{
	emscripten_fiber_init_from_current_context(&HostFiber, HostAsyncStack.data(), HostAsyncStack.size());
	emscripten_fiber_init(&GameFiber, Run, nullptr, GameStack.data(), GameStack.size(), GameAsyncStack.data(), GameAsyncStack.size());
	emscripten_fiber_swap(&HostFiber, &GameFiber);
	std::printf("browser field parser: %u checks, %u failures\n", Checks, Failures);
	return Failures == 0 ? 0 : 1;
}
