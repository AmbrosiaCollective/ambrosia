void tib_checks() {
    using namespace ambrosia;
    const auto bounds = host::current_stack_bounds();
    std::byte marker{};
    const auto marker_address = reinterpret_cast<std::uintptr_t>(&marker);
    check(bounds.lower && bounds.lower < bounds.upper && marker_address >= bounds.lower &&
              marker_address < bounds.upper,
          "dynamically queried native stack bounds contain the current frame");
    const auto again = host::current_stack_bounds();
    check(bounds.lower == again.lower && bounds.upper == again.upper,
          "native stack discovery is stable across calls on one thread");
#if defined(__linux__) && defined(__x86_64__)
    guest::ThreadContext outer, inner;
    const auto original_gs = host::guest_segment_base();
    auto read_self = [] {
        std::uintptr_t self = 0;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(self));
        return self;
    };
    auto read_bounds = [] {
        host::StackBounds stack;
        __asm__ volatile("movq %%gs:0x10, %0" : "=r"(stack.lower));
        __asm__ volatile("movq %%gs:0x08, %0" : "=r"(stack.upper));
        return stack;
    };
    std::uintptr_t outer_self = 0;
    {
        auto active = outer.activate();
        outer_self = read_self();
        check(outer_self && outer_self == host::guest_segment_base(),
              "guest TIB Self points to the current GS anchor");
        const auto stack = read_bounds();
        check(stack.lower == bounds.lower && stack.upper == bounds.upper,
              "guest TIB stores real host lower/upper stack bounds at x64 ABI offsets");
        std::uintptr_t indirect_base = 0;
        if (outer_self) {
            std::memcpy(&indirect_base, reinterpret_cast<const std::byte *>(outer_self) + 8, 8);
        }
        check(indirect_base == bounds.upper,
              "generic TIB self traversal reaches the same dynamically determined stack base");
        {
            auto nested = inner.activate();
            check(read_self() && read_self() != outer_self && read_bounds().upper == bounds.upper,
                  "nested guest contexts have distinct Self pointers on the same native stack");
        }
        check(read_self() == outer_self, "leaving nested context restores prior TIB Self");
    }
    check(host::guest_segment_base() == original_gs, "TIB scopes restore original native GS");
#endif
}
