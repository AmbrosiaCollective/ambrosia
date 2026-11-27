struct CreationObservation {
    ambrosia::services::Runtime *runtime = nullptr;
    ambrosia::guest::ThreadContext *context = nullptr;
    std::uintptr_t instance = 0, procedure = 0, parameter = 0, handle = 0;
    std::thread::id thread;
    std::vector<std::uint32_t> messages;
    bool valid = true;
    unsigned reject = 0;
    unsigned default_mode = 0;
};

CreationObservation *active_creation = nullptr;

std::intptr_t AMBROSIA_GUEST_ABI creation_procedure(std::uintptr_t handle, std::uint32_t message,
                                                    std::uintptr_t first, std::intptr_t second) {
    using namespace ambrosia;
    using namespace services;
    auto &observation = *active_creation;
    auto window = observation.runtime->windows().find(handle);
    observation.handle = handle;
    observation.messages.push_back(message);
    observation.valid =
        observation.valid && window && current_runtime() == observation.runtime &&
        guest::ThreadContext::current() == observation.context &&
        std::this_thread::get_id() == observation.thread && !first && current_modules() &&
        current_modules()->module_base_for_address(observation.procedure) == observation.instance;
    if (message == 0x81 || message == 1) {
        const auto *bytes = reinterpret_cast<const std::byte *>(second);
        std::uint64_t parameter, instance, parent, menu;
        std::int32_t height, width;
        std::memcpy(&parameter, bytes, 8);
        std::memcpy(&instance, bytes + 8, 8);
        std::memcpy(&menu, bytes + 16, 8);
        std::memcpy(&parent, bytes + 24, 8);
        std::memcpy(&height, bytes + 32, 4);
        std::memcpy(&width, bytes + 36, 4);
        observation.valid = observation.valid && parameter == observation.parameter &&
                            instance == observation.instance && !parent && !menu && width == 640 &&
                            height == 360;
    } else if (message == 0x83) {
        auto *rectangle = reinterpret_cast<std::int32_t *>(second);
        observation.valid =
            observation.valid && rectangle[2] - rectangle[0] == 640 && rectangle[3] - rectangle[1] == 360;
        rectangle[0] += 3;
        rectangle[1] += 4;
        rectangle[2] -= 3;
        rectangle[3] -= 4;
    }
    errno = ERANGE;
    crt_error() = 31;
    set_last_error(211);
    if (message == 0x81) {
        if (observation.default_mode) {
            using Default = std::intptr_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::uint32_t, std::uintptr_t,
                                                                std::intptr_t);
            const auto procedure = reinterpret_cast<Default>(
                observation.runtime->resolve(service_symbol("DefWindowProcA", "user32.dll")));
            if (observation.default_mode == 2) {
                return procedure(handle, message, first, second + 8);
            }
            if (observation.default_mode == 3) {
                return procedure(handle, 0x1234, 0, 0);
            }
            observation.valid = observation.valid && !procedure(handle, message, 0, 0);
            const char *changed_title = "Original default title \xc3\xa9";
            std::memcpy(reinterpret_cast<std::byte *>(second) + 56, &changed_title, sizeof(changed_title));
            return procedure(handle, message, first, second);
        }
        return observation.reject == 1 ? 0 : 1;
    }
    return message == 1 && observation.reject == 2 ? -1 : 0;
}

void window_creation_checks(unsigned default_probe = 0) {
    using namespace ambrosia;
    using namespace services;
    Runtime runtime;
    using Create = std::uintptr_t(AMBROSIA_GUEST_ABI *)(
        std::uint32_t, std::uintptr_t, const char *, std::uint32_t, std::int32_t, std::int32_t, std::int32_t,
        std::int32_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, void *);
    const auto address = runtime.resolve(service_symbol("CreateWindowExA", "user32.dll"));
    check(address && address == runtime.resolve(service_symbol(
                                    "CreateWindowExA", "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll")),
          "native window creation aliases expose the actual twelve-argument Windows x64 service");
    if (!address) {
        return;
    }
    const auto default_address = runtime.resolve(service_symbol("DefWindowProcA", "user32.dll"));
    check(default_address &&
              default_address == runtime.resolve(service_symbol(
                                     "DefWindowProcA", "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll")),
          "default processing aliases expose the same typed Windows ABI service");
    runtime.windows().bind("/presentation");
    auto bytes = fixture();
    put<std::uint32_t>(bytes, 0x98 + 120, 0);
    put<std::uint32_t>(bytes, 0x98 + 124, 0);
    jump_to_host(bytes, 0x300, reinterpret_cast<std::uintptr_t>(&creation_procedure));
    pe::ModuleGraph graph;
    graph.add("window-owner.exe", bytes);
    graph.load("window-owner.exe");
    Scope scope(runtime, &graph);
    guest::ThreadContext context;
    auto activation = context.activate();
    WindowClassDescription description;
    description.name = "OriginalWindow";
    description.procedure = graph.loaded_module() + 0x1100;
    description.instance = graph.loaded_module();
    description.window_extra_bytes = 16;
    description.modules = std::make_shared<pe::ModuleGraph>(graph);
    const auto registered = runtime.window_classes().add(description);
    check(registered.atom && !registered.error,
          "the creation fixture uses an owned class whose callback belongs to the mapped guest catalog");
    CreationObservation observation;
    observation.runtime = &runtime;
    observation.context = &context;
    observation.instance = description.instance;
    observation.procedure = description.procedure;
    observation.parameter = 0x123456789abcdef0ull;
    observation.thread = std::this_thread::get_id();
    observation.default_mode = default_probe;
    active_creation = &observation;
    const auto create = reinterpret_cast<Create>(address);
    const auto call = [&](std::uintptr_t class_id) {
        return create(0, class_id, "Ambrosia creation check", 0xcf0000, INT32_MIN, INT32_MIN, 640, 360, 0, 0,
                      description.instance, reinterpret_cast<void *>(observation.parameter));
    };
    const auto saved_error = errno;
    errno = E2BIG;
    crt_error() = 19;
    set_last_error(83);
    const auto handle = call(reinterpret_cast<std::uintptr_t>("originalwindow"));
    auto window = runtime.windows().find(handle);
    check(handle && window && observation.valid && observation.handle == handle &&
              observation.messages == std::vector<std::uint32_t>{0x81, 0x83, 1},
          "a real hidden host window receives orderedNCCREATE/NCCALCSIZE/CREATE callbacks on the caller's "
          "guest context");
    if (!window) {
        active_creation = nullptr;
        errno = saved_error;
        return;
    }
    check(window->host_window.id && window->owner == observation.thread && window->extra.size() == 16 &&
              std::all_of(window->extra.begin(), window->extra.end(),
                          [](auto value) { return value == std::byte{0}; }) &&
              window->client_rectangle[2] - window->client_rectangle[0] == 634 &&
              window->client_rectangle[3] - window->client_rectangle[1] == 352,
          "the guest handle retains owned host/class/extra metadata and the callback's client rectangle");
    check(errno == E2BIG && crt_error() == 31 && get_last_error() == 211,
          "window creation preserves host errno while retaining guest error changes made by its procedure");
    runtime.windows().retire(handle);
    check(!runtime.windows().find(handle) && window->host_window.id,
          "retiring a guest handle removes lookup while an explicit metadata lease remains alive");
    window.reset();
    observation.messages.clear();
    observation.reject = 1;
    check(call(registered.atom) == 0 && observation.messages == std::vector<std::uint32_t>{0x81} &&
              !runtime.windows().find(observation.handle),
          "a rejectedNCCREATE returns no guest window and retires the real host ownership");
    observation.messages.clear();
    observation.reject = 2;
    check(call(registered.atom) == 0 &&
              observation.messages == std::vector<std::uint32_t>{0x81, 0x83, 1, 2, 0x82} &&
              !runtime.windows().find(observation.handle),
          "a rejectedCREATE delivers destruction notifications and rolls back the owned window");
    observation.messages.clear();
    observation.reject = 0;
    check(call(7) == 0 && get_last_error() == 1407 && observation.messages.empty(),
          "an unknown class fails before a host window or guest callback exists");
    check(create(0, registered.atom, "invalid", 0xcf0000, 0, 0, 0, 360, 0, 0, description.instance,
                 nullptr) == 0 &&
              get_last_error() == error_invalid_parameter && observation.messages.empty(),
          "invalid window dimensions fail without creating native or guest state");
    auto caller = fixture();
    text(caller, 0x440, "user32.dll");
    text(caller, 0x4a2, "CreateWindowExA");
    std::vector<std::byte> code;
    const auto emit = [&](std::initializer_list<unsigned char> values) {
        for (auto value : values) {
            code.push_back(std::byte{value});
        }
    };
    const auto immediate = [&](std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index) {
            code.push_back(std::byte((value >> (index * 8)) & 255));
        }
    };
    emit({0x48, 0x83, 0xec, 0x68, 0x31, 0xc9});
    emit({0x48, 0xba});
    immediate(registered.atom);
    emit({0x49, 0xb8});
    immediate(reinterpret_cast<std::uintptr_t>("OriginalPEWindow"));
    emit({0x41, 0xb9, 0, 0, 0xcf, 0});
    const std::array<std::uint64_t, 8> stack_arguments{
        0xfeed000080000000ull, 0xcafe000080000000ull, 640, 360, 0, 0,
        description.instance,  observation.parameter};
    for (std::size_t index = 0; index < stack_arguments.size(); ++index) {
        emit({0x48, 0xb8});
        immediate(stack_arguments[index]);
        emit({0x48, 0x89, 0x44, 0x24, static_cast<unsigned char>(0x20 + index * 8)});
    }
    emit({0xff, 0x15});
    const auto displacement = code.size();
    emit({0, 0, 0, 0, 0x48, 0x83, 0xc4, 0x68, 0xc3});
    std::memcpy(caller.data() + 0x200, code.data(), code.size());
    put<std::uint32_t>(caller, 0x200 + displacement,
                       static_cast<std::uint32_t>(0x2080 - (0x1000 + displacement + 4)));
    auto image = pe::Image::load(caller, [&](const auto &symbol) { return runtime.resolve(symbol); });
    const auto pe_handle = image.run_bootstrap();
    check(pe_handle && runtime.windows().find(pe_handle) && observation.valid &&
              observation.messages == std::vector<std::uint32_t>{0x81, 0x83, 1},
          "an original PE supplies all twelve register/stack arguments, including64-bit instance and "
          "creation parameter");
    runtime.windows().retire(pe_handle);
    if (default_address) {
        observation.default_mode = 1;
        observation.messages.clear();
        const auto default_handle = call(registered.atom);
        auto default_window = runtime.windows().find(default_handle);
        check(
            default_window && observation.valid &&
                default_window->title == "Original default title \xc3\xa9" &&
                !default_window->creation_structure &&
                observation.messages == std::vector<std::uint32_t>{0x81, 0x83, 1},
            "default NCCREATE updates the real host title before acceptance and clears its structure lease");
        using Default =
            std::intptr_t(AMBROSIA_GUEST_ABI *)(std::uintptr_t, std::uint32_t, std::uintptr_t, std::intptr_t);
        const auto procedure = reinterpret_cast<Default>(default_address);
        runtime.windows().retire(default_handle);
        default_window.reset();
        check(!procedure(default_handle, 0x81, 0, 0) && get_last_error() == 1400,
              "default processing rejects retired handles before inspecting a creation pointer");
    }
    active_creation = nullptr;
    errno = saved_error;
}
