#pragma once
#include <iostream>
#define CHECK(expression) do { if (!(expression)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " << #expression << '\n'; ++failures; } } while (0)
