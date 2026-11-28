// A plain C runtime (/MD) program: when main returns, the C runtime's start-up
// code calls exit with its result, which OpenEra must catch, running the
// program's atexit handlers but leaving the host's C runtime alone.
#include <cstdlib>
#include <tuple>

#include <Windows.h>

namespace
{
void OnExit() noexcept
{
    OutputDebugStringA("CrtExitImage: atexit handler ran\n");
}
} // namespace

int main()
{
    std::ignore = std::atexit(&OnExit);
    return 3;
}
