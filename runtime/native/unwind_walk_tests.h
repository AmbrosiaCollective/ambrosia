void unwind_walk_checks() {
    using namespace ambrosia::pe;
    auto image = program_fixture();
    const RuntimeFunction function{0x100, 0x180, 0x200};
    Bytes stack(0x100);
    constexpr std::uint64_t base = 0x10000;
    const StackView view{base, stack};
    RegisterContext context;
    context.integer[4] = base;
    put<std::uint64_t>(stack, 32, 0x1234);
    put<std::uint64_t>(stack, 40, 0x5678);
    auto straight = restore_chained_prologue(image, function, context, view, 20);
    check(straight.context.rip == 0x5678 && straight.context.integer[3] == 0x1234,
          "chain walker agrees with unchained prologue restoration");
    auto chain = image;
    chain[0x200] = std::byte{33}; // chain with allocation in this fragment
    chain[0x202] = std::byte{1};
    put<std::uint32_t>(chain, 0x208, 0x100);
    put<std::uint32_t>(chain, 0x20c, 0x180);
    put<std::uint32_t>(chain, 0x210, 0x240);
    chain[0x240] = std::byte{1};
    chain[0x241] = std::byte{1};
    chain[0x242] = std::byte{1};
    chain[0x244] = std::byte{1};
    chain[0x245] = std::byte{0x30};
    const auto restored = restore_chained_prologue(chain, function, context, view, 20);
    check(restored.context.rip == 0x5678 && restored.context.integer[3] == 0x1234 &&
              restored.context.integer[4] == base + 48,
          "chained allocations and saves pop the return address only once");
    auto cycle = chain;
    put<std::uint32_t>(cycle, 0x210, 0x200);
    check(runtime_rejected([&] { (void)restore_chained_prologue(cycle, function, context, view, 20); }),
          "unwind chain cycles fail without touching stack/context");
    auto inconsistent = chain;
    inconsistent[0x243] = std::byte{5};
    check(
        runtime_rejected([&] { (void)restore_chained_prologue(inconsistent, function, context, view, 20); }),
        "inconsistent chained frame registers fail explicitly");

    auto code = image;
    const unsigned char epilogue[] = {0x48, 0x83, 0xc4, 32, 0x5b, 0xc3};
    std::memcpy(code.data() + 0x160, epilogue, sizeof(epilogue));
    const auto program = UnwindProgram::decode(code, function);
    auto epilogue_result = restore_epilogue(code, function, program, 0x160, context, view);
    check(epilogue_result && epilogue_result->context.rip == 0x5678 &&
              epilogue_result->context.integer[3] == 0x1234 &&
              epilogue_result->context.integer[4] == base + 48,
          "epilogue reverses only remaining add/pop/return instructions");
    context.integer[4] = base + 32;
    auto after_add = restore_epilogue(code, function, program, 0x164, context, view);
    check(after_add && after_add->context.rip == 0x5678 && after_add->context.integer[3] == 0x1234,
          "epilogue PC after stack adjustment does not adjust it twice");
    context.integer[4] = base + 40;
    auto at_ret = restore_epilogue(code, function, program, 0x165, context, view);
    check(at_ret && at_ret->context.rip == 0x5678 && at_ret->context.integer[3] == 0,
          "epilogue PC at return does not restore an already popped register");
    check(!restore_epilogue(code, function, program, 0x120, context, view),
          "ordinary body bytes are not claimed as an epilogue");
    check(!restore_epilogue(code, function, program, 0x100, context, view),
          "prologue bytes are not interpreted as an epilogue");
    check(runtime_rejected([&] { (void)restore_epilogue(code, function, program, 0x180, context, view); }),
          "epilogue PC must remain inside function");
    check(runtime_rejected(
              [&] { (void)restore_epilogue(code, function, program, 0x165, context, {base, {}}); }),
          "epilogue stack read respects supplied bounds");
    check(context.integer[4] == base + 40 && context.rip == 0,
          "epilogue success/failure never modifies the input context");

    const unsigned char lea[] = {0x48, 0x8d, 0x65, 0xe0, 0x5d, 0xf3, 0xc3};
    std::memcpy(code.data() + 0x160, lea, sizeof(lea));
    auto frame_program = program;
    frame_program.frame_register = 5;
    context.integer[5] = base + 64;
    auto lea_result = restore_epilogue(code, function, frame_program, 0x160, context, view);
    check(lea_result && lea_result->context.integer[5] == 0x1234 && lea_result->context.rip == 0x5678,
          "LEA frame restoration applies signed displacement before pop/rep-return");
    const unsigned char extended[] = {0x41, 0x5c, 0xc2, 16, 0};
    std::memcpy(code.data() + 0x160, extended, sizeof(extended));
    context.integer[4] = base + 32;
    auto extended_result = restore_epilogue(code, function, program, 0x160, context, view);
    check(extended_result && extended_result->context.integer[12] == 0x1234 &&
              extended_result->context.rip == 0x5678 && extended_result->context.integer[4] == base + 64,
          "extended-register pop and immediate return restore caller stack");
    code[0x160] = std::byte{0xeb};
    code[0x161] = std::byte{0xfe};
    check(!restore_epilogue(code, function, program, 0x160, context, view),
          "backward jump inside the same function is not a tail epilogue");
    code[0x161] = std::byte{0x40};
    context.integer[4] = base + 40;
    auto tail = restore_epilogue(code, function, program, 0x160, context, view);
    check(tail && tail->context.rip == 0x5678, "jump outside current function unwinds as a tail call");
    code[0x160] = std::byte{0xff};
    code[0x161] = std::byte{0x25};
    auto indirect = restore_epilogue(code, function, program, 0x160, context, view);
    check(indirect && indirect->context.rip == 0x5678,
          "indirect tail epilogue does not dereference or execute its target");
    code[0x17f] = std::byte{0x41};
    check(!restore_epilogue(code, function, program, 0x17f, context, view),
          "truncated epilogue instruction fails recognition safely");
}
