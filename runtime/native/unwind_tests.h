Bytes unwind_fixture() {
    Bytes bytes(3 * 12);
    // One empty record followed by two separated executable ranges.
    const std::array<ambrosia::pe::RuntimeFunction, 3> entries{
        {{0x1000, 0x1000, 0x2000}, {0x1000, 0x1020, 0x2004}, {0x1040, 0x1080, 0x2010}}};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        put<std::uint32_t>(bytes, index * 12, entries[index].begin);
        put<std::uint32_t>(bytes, index * 12 + 4, entries[index].end);
        put<std::uint32_t>(bytes, index * 12 + 8, entries[index].unwind);
    }
    return bytes;
}

void unwind_checks() {
    using ambrosia::pe::UnwindTable;
    const auto bytes = unwind_fixture();
    const auto table = UnwindTable::parse(bytes, 0x3000);
    check(table.entries().size() == 3, "owned unwind table retains zero-length records");
    check(table.find(0x1000) == &table.entries()[1],
          "unwind lookup selects covering function after an empty record");
    check(table.find(0x101f) == &table.entries()[1], "unwind lookup covers final byte of function");
    check(!table.find(0x1020), "unwind lookup excludes exact function end");
    check(!table.find(0x103f), "unwind lookup leaves gaps uncovered");
    check(table.find(0x1040) == &table.entries()[2], "unwind lookup includes next function start");
    check(!table.find(0x1080) && !table.find(0x0fff), "unwind lookup rejects both outside boundaries");
    auto changed = bytes;
    put<std::uint32_t>(changed, 12, 0);
    check(table.find(0x1000)->begin == 0x1000, "parsed unwind table does not borrow caller bytes");
    check(runtime_rejected([&] { (void)UnwindTable::parse(Bytes(13), 0x3000); }),
          "partial unwind record fails explicitly");
    check(runtime_rejected([&] { (void)UnwindTable::parse(bytes, 0); }), "zero image extent is invalid");
    check(!UnwindTable::parse({}, 0x3000).find(0x1000), "empty unwind directory has no matching function");
    auto reject_record = [&](std::size_t offset, std::uint32_t value) {
        auto malformed = bytes;
        put<std::uint32_t>(malformed, offset, value);
        return runtime_rejected([&] { (void)UnwindTable::parse(malformed, 0x3000); });
    };
    check(reject_record(12 + 4, 0x0fff), "reversed unwind function range fails");
    check(reject_record(24 + 4, 0x3001), "unwind function cannot exceed image extent");
    check(reject_record(24, 0x1010), "overlapping or unordered unwind functions fail");
    check(reject_record(12 + 8, 0x3000), "unwind header must fit inside image extent");
    check(reject_record(12 + 8, 0x2002), "unaligned unwind header fails");
    check(reject_record(12 + 8, 0), "missing unwind header fails");
    check(reject_record(12 + 8, 0x2001), "indirect unwind record is explicitly unsupported");
}
