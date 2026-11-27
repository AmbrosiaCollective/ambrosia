void presentation_checks() {
    using namespace ambrosia;
    host::Presentation presentation("/presentation");
    check(runtime_rejected([&] { host::Presentation duplicate("/presentation"); }),
          "the explicit presentation endpoints admit only one live native client owner");
    host::WindowRequest request;
    request.title = "Ambrosia native presentation check";
    request.geometry = {INT32_MIN, INT32_MIN, 640, 360};
    request.visible = true;
    request.resizable = true;
    const auto window = presentation.create(request);
    check(window.id && window.geometry.width == 640 && window.geometry.height == 360,
          "a bounded file request creates a real owned SDL host window with reported geometry");
    std::vector<std::byte> rgba(640 * 360 * 4);
    for (std::size_t index = 0; index < rgba.size(); index += 4) {
        rgba[index] = std::byte{0x20};
        rgba[index + 1] = std::byte{0x80};
        rgba[index + 2] = std::byte{0x60};
        rgba[index + 3] = std::byte{0xff};
    }
    presentation.present(window.id, 640, 360, rgba);
    const auto hidden = presentation.show(window.id, false);
    const auto shown = presentation.show(window.id, true);
    check(hidden.width == 640 && shown.height == 360,
          "copied synthetic RGBA pixels and visibility changes reach the same owned software window");
    const auto events = presentation.poll();
    check(events.size() <= 64 && std::all_of(events.begin(), events.end(),
                                             [&](const auto &event) { return event.window == window.id; }),
          "the presentation service returns bounded normalized events only for its owned window");
    auto invalid = request;
    invalid.geometry.width = 0;
    check(runtime_rejected([&] { (void)presentation.create(invalid); }) &&
              runtime_rejected([&] { presentation.present(window.id, 640, 360, std::span(rgba).first(1)); }),
          "invalid dimensions and mismatched frame sizes fail before an unbounded transport write");
    invalid = request;
    invalid.title = std::string("bad\0title", 9);
    check(runtime_rejected([&] { (void)presentation.create(invalid); }),
          "an embeddedNUL title cannot silently change the window request contract");
    auto large = request;
    large.geometry.width = 3840;
    large.geometry.height = 2160;
    large.visible = false;
    const auto full_size = presentation.create(large);
    check(full_size.id != window.id && full_size.geometry.width == 3840 && full_size.geometry.height == 2160,
          "the measured4K window dimensions fit the explicit host service budget");
    presentation.destroy(full_size.id);
    presentation.destroy(window.id);
    check(runtime_rejected([&] { (void)presentation.show(window.id, true); }),
          "a destroyed presentation window cannot be addressed by a stale owned ID");
}
