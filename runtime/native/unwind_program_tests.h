Bytes program_fixture() {
    Bytes image(0x400);
    image[0x200] = std::byte{1}; // version one
    image[0x201] = std::byte{5}; // prologue length
    image[0x202] = std::byte{2}; // two slots
    image[0x204] = std::byte{5};
    image[0x205] = std::byte{0x32}; // sub rsp,32
    image[0x206] = std::byte{1};
    image[0x207] = std::byte{0x30}; // push rbx
    return image;
}

void unwind_program_checks() {
    using namespace ambrosia::pe;
    const RuntimeFunction function{0x100, 0x180, 0x200};
    auto image = program_fixture();
    auto program = UnwindProgram::decode(image, function);
    check(program.operations.size() == 2 && program.operations[0].operand == 32,
          "decode checked small allocation and nonvolatile push");
    Bytes stack(0x100);
    constexpr std::uint64_t stack_base = 0x10000;
    put<std::uint64_t>(stack, 0x20, 0x3344);
    put<std::uint64_t>(stack, 0x28, 0x5566);
    RegisterContext context;
    context.integer[4] = stack_base;
    context.integer[0] = 123;
    const auto original = context;
    const StackView view{stack_base, stack};
    auto restored = restore_prologue(program, context, view, 0x20);
    check(restored.context.integer[3] == 0x3344 && restored.context.integer[4] == stack_base + 0x30 &&
              restored.context.rip == 0x5566 && restored.context.integer[0] == 123,
          "body unwind reverses allocation and push then pops return address");
    check(restored.establisher_frame == stack_base && !restored.in_prologue &&
              context.integer == original.integer && context.rip == original.rip,
          "unwind output preserves input context and reports establisher frame");
    context.integer[4] = stack_base + 0x20;
    auto partial = restore_prologue(program, context, view, 1);
    check(partial.in_prologue && partial.context.integer[3] == 0x3344 && partial.context.rip == 0x5566,
          "partial prologue skips allocation that has not executed");
    context.integer[4] = stack_base + 0x28;
    auto before = restore_prologue(program, context, view, 0);
    check(before.context.integer[3] == 0 && before.context.rip == 0x5566,
          "prologue start skips every unexecuted unwind action");
    check(runtime_rejected([&] { (void)restore_prologue(program, context, {stack_base, {}}, 5); }),
          "unwind stack reads cannot escape supplied extent");
    check(context.integer[4] == stack_base + 0x28, "failed restoration leaves original context unchanged");

    auto rejects = [&](std::size_t offset, unsigned value) {
        auto malformed = image;
        malformed[offset] = std::byte(value);
        return runtime_rejected([&] { (void)UnwindProgram::decode(malformed, function); });
    };
    check(rejects(0x200, 0), "unknown unwind version fails explicitly");
    check(rejects(0x200, 1 | (8 << 3)), "unknown unwind flags fail explicitly");
    check(rejects(0x200, 1 | (5 << 3)), "chained and handler flags cannot be combined");
    check(rejects(0x203, 4), "stack pointer is not a valid frame register");
    check(rejects(0x203, 0x10), "frame offset requires a frame register");
    check(rejects(0x205, 0x11), "large allocation cannot consume missing operand slots");
    check(rejects(0x205, 0x06), "unimplemented epilogue opcode is not silently skipped");
    check(rejects(0x207, 0), "volatile register push is not a valid unwind save");
    check(rejects(0x206, 6), "unwind operations must be in descending code-offset order");
    check(runtime_rejected([&] { (void)UnwindProgram::decode(Bytes(0x203), function); }),
          "truncated unwind header fails");

    auto large = image;
    large[0x202] = std::byte{3};
    large[0x205] = std::byte{1}; // ALLOC_LARGE, scaled u16 operand
    put<std::uint16_t>(large, 0x206, 4);
    large[0x208] = std::byte{1};
    large[0x209] = std::byte{0x30};
    context.integer[4] = stack_base;
    auto scaled = UnwindProgram::decode(large, function);
    check(restore_prologue(scaled, context, view, 20).context.rip == 0x5566,
          "scaled large stack allocation restores correct caller");
    large[0x202] = std::byte{4};
    large[0x205] = std::byte{0x11};
    put<std::uint32_t>(large, 0x206, 32);
    large[0x20a] = std::byte{1};
    large[0x20b] = std::byte{0x30};
    check(restore_prologue(UnwindProgram::decode(large, function), context, view, 20).context.rip == 0x5566,
          "unscaled large allocation restores correct caller");
    put<std::uint32_t>(large, 0x206, 31);
    check(runtime_rejected([&] { (void)UnwindProgram::decode(large, function); }),
          "large allocation rejects unaligned sizes");

    auto framed = image;
    framed[0x201] = std::byte{8};
    framed[0x202] = std::byte{3};
    framed[0x203] = std::byte{0x25}; // RBP points 32 bytes past establisher.
    framed[0x204] = std::byte{8};
    framed[0x205] = std::byte{3};
    framed[0x206] = std::byte{5};
    framed[0x207] = std::byte{0x32};
    framed[0x208] = std::byte{1};
    framed[0x209] = std::byte{0x50};
    context.integer[5] = stack_base + 32;
    context.integer[4] = stack_base - 8;
    auto frame = restore_prologue(UnwindProgram::decode(framed, function), context, view, 20);
    check(frame.establisher_frame == stack_base && frame.context.integer[5] == 0x3344 &&
              frame.context.rip == 0x5566,
          "frame register restores RSP before allocation and saved RBP");
    context.integer[4] = stack_base;
    auto frame_before_set = restore_prologue(UnwindProgram::decode(framed, function), context, view, 5);
    check(frame_before_set.establisher_frame == stack_base && frame_before_set.context.rip == 0x5566,
          "partial prologue does not use an unestablished frame register");

    auto saves = image;
    saves[0x202] = std::byte{5};
    saves[0x204] = std::byte{5};
    saves[0x205] = std::byte{0x64}; // SAVE_NONVOL RSI at frame+8.
    put<std::uint16_t>(saves, 0x206, 1);
    saves[0x208] = std::byte{4};
    saves[0x209] = std::byte{0x68}; // SAVE_XMM128 XMM6 at frame+16.
    put<std::uint16_t>(saves, 0x20a, 1);
    saves[0x20c] = std::byte{1};
    saves[0x20d] = std::byte{0x32};
    put<std::uint64_t>(stack, 8, 0x7788);
    stack[16] = std::byte{0xaa};
    put<std::uint64_t>(stack, 32, 0x99aa);
    auto saved = restore_prologue(UnwindProgram::decode(saves, function), context, view, 20);
    check(saved.context.integer[6] == 0x7788 && saved.context.xmm[6][0] == std::byte{0xaa} &&
              saved.context.rip == 0x99aa,
          "integer and XMM saves use establisher-relative scaled offsets");

    auto handler = image;
    handler[0x200] = std::byte{9}; // EHANDLER
    put<std::uint32_t>(handler, 0x208, 0x140);
    const auto with_handler = UnwindProgram::decode(handler, function);
    check(with_handler.handler_rva == 0x140 && with_handler.handler_data_rva == 0x20c,
          "decode handler address and aligned opaque language-data position");
    put<std::uint32_t>(handler, 0x208, 0x400);
    check(runtime_rejected([&] { (void)UnwindProgram::decode(handler, function); }),
          "handler address must remain within image");
    auto chained = image;
    chained[0x200] = std::byte{33};
    put<std::uint32_t>(chained, 0x208, 0x100);
    put<std::uint32_t>(chained, 0x20c, 0x180);
    put<std::uint32_t>(chained, 0x210, 0x240);
    const auto chain = UnwindProgram::decode(chained, function);
    check(chain.chained && chain.chained->unwind == 0x240, "decode bounded chained function record");
    check(runtime_rejected([&] { (void)restore_prologue(chain, context, view, 20); }),
          "unprocessed chain cannot produce a fake caller context");

    auto machine = image;
    machine[0x201] = std::byte{0};
    machine[0x202] = std::byte{1};
    machine[0x204] = std::byte{0};
    machine[0x205] = std::byte{0x1a};
    put<std::uint64_t>(stack, 8, 0x1234);
    put<std::uint64_t>(stack, 32, stack_base + 64);
    const auto restored_machine =
        restore_prologue(UnwindProgram::decode(machine, function), context, view, 20);
    check(restored_machine.context.rip == 0x1234 && restored_machine.context.integer[4] == stack_base + 64,
          "machine frame accounts for error code and restores saved RIP/RSP");
    context.integer[4] = UINT64_MAX - 8;
    check(runtime_rejected([&] { (void)restore_prologue(program, context, view, 20); }),
          "stack allocation restoration rejects address overflow");
}
