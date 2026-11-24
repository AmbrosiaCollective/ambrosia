void limit_checks() {
    using ambrosia::pe::Image;
    using ambrosia::pe::ImageLimits;
    using ambrosia::pe::ModuleGraph;
    using ambrosia::pe::ModuleLimits;
    const auto bytes = fixture();
    ImageLimits image_limits;
    image_limits.file_bytes = bytes.size();
    image_limits.mapped_bytes = 0x4000;
    check(Image::load(bytes, resolve, 0, image_limits).run_bootstrap() == 21,
          "exact configured file and mapping budgets permit a valid image");
    --image_limits.file_bytes;
    check(runtime_rejected([&] { (void)Image::load(bytes, resolve, 0, image_limits); }),
          "per-image file budget rejects one byte over the limit");
    image_limits.file_bytes = bytes.size();
    --image_limits.mapped_bytes;
    check(runtime_rejected([&] { (void)Image::load(bytes, resolve, 0, image_limits); }),
          "per-image mapping budget is checked before mapping");
    image_limits.mapped_bytes = 0;
    check(runtime_rejected([&] { (void)Image::load(bytes, resolve, 0, image_limits); }),
          "zero image mapping budget cannot remove the limit");

    ModuleLimits limits;
    limits.source_bytes = bytes.size();
    limits.mapped_bytes = 0x4000;
    limits.image.file_bytes = bytes.size();
    limits.image.mapped_bytes = 0x4000;
    ModuleGraph exact(resolve, {}, limits);
    exact.add("app.exe", bytes);
    const Bytes extra{std::byte{0}};
    check(runtime_rejected([&] { exact.add("extra.dll", extra); }),
          "graph aggregate source budget is enforced during registration");
    exact.load("app.exe");
    check(exact.statistics().mapped_bytes == 0x4000 && exact.start().run_bootstrap() == 21,
          "graph threads configured budgets through the actual image loader");
    --limits.mapped_bytes;
    ModuleGraph too_small(resolve, {}, limits);
    too_small.add("app.exe", bytes);
    check(runtime_rejected([&] { too_small.load("app.exe"); }),
          "graph aggregate mapping budget also constrains the root image");
    limits.source_bytes = 0;
    bool invalid_limits = false;
    try {
        ModuleGraph invalid(resolve, {}, limits);
    } catch (const std::invalid_argument &) {
        invalid_limits = true;
    }
    check(invalid_limits, "zero graph budget fails at configuration time");

    auto unwind = bytes;
    put<std::uint32_t>(unwind, 0x98 + 136, 0x2100);
    put<std::uint32_t>(unwind, 0x98 + 140, 12);
    std::string diagnostic;
    try {
        (void)Image::load(unwind, resolve);
    } catch (const std::runtime_error &error) {
        diagnostic = error.what();
    }
    check(diagnostic == "unsupported PE directory 3: exception/unwind",
          "unsupported directory diagnostic identifies the missing service");
}
