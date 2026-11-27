// Only its import library is used: the DLL itself is kept out of the fixture directory.
extern "C" __declspec(dllexport) int DelayMissingValue() noexcept
{
    return 1;
}
