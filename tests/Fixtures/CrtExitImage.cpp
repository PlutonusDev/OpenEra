// A plain C runtime (/MD) program: when main returns, the C runtime's start-up
// code calls exit with its result, which OpenEra must catch.
int main()
{
    return 3;
}
