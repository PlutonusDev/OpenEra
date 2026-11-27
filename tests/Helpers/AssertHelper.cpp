// Fails an assertion on request, so the tests can check what a failed
// assertion does to a process: AssertHelper assert|verify|pass.
#include <exception>
#include <span>
#include <string_view>

#include "Core/Assert.h"

namespace
{
int Twice(int value) noexcept
{
    return value * 2;
}

void Run(std::string_view mode)
{
    if (mode == "assert")
    {
        const int value = 42;
        OPENERA_ASSERT(Twice(value) == 0, "Twice({}) is {}", value, Twice(value));
    }
    else if (mode == "verify")
    {
        OPENERA_VERIFY(Twice(1) == 3);
    }
    else
    {
        OPENERA_ASSERT(Twice(2) == 4);
        OPENERA_VERIFY(Twice(3) == 6, "never shown");
    }
}
} // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): the program entry point.
int main(int argc, char** argv)
{
    try
    {
        const std::span<char*> arguments(argv, argc);
        Run(arguments.size() > 1 ? std::string_view(arguments.subspan(1).front()) : std::string_view{});
        return 0;
    }
    catch (const std::exception&)
    {
        return 1;
    }
}
