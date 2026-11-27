<!-- Please read CONTRIBUTING.md first: clean-room rules, the checks CI runs, and commit message format. -->

## Summary
<!-- What changed and why. Link related issues (e.g. "Closes #12"). -->

## Reviewer notes
<!-- Design decisions, trade-offs, or anything that needs a closer look. Delete if none. -->

## Testing
<!-- How you verified the change: new/updated tests, compilers used, manual checks. -->

## Checklist
- [ ] Clean-room: only public documentation, observable behaviour, or compatibly licensed open source (attributed) was used. No leaked code, disassembly, SDK headers or keys. See [CONTRIBUTING.md](https://github.com/PlutonusDev/OpenEra/blob/main/CONTRIBUTING.md#clean-room-policy).
- [ ] Commit messages use `subsystem: summary`.
- [ ] `clang-format` applied to changed `.h` / `.cpp` files.
- [ ] Builds with zero warnings under MSVC (`/W4 /WX`, C++ Core Check) and Clang-CL.
- [ ] `ctest --test-dir build --output-on-failure` passes.
- [ ] New parsers or binary structs have unit tests and `static_assert` size checks.
