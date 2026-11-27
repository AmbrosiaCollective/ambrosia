void shared_backing_checks(int descriptor) {
#ifdef __linux__
    using namespace ambrosia;
    constexpr std::size_t page = 65536;
    constexpr std::size_t capacity = 1024 * 1024 * 1024;
    std::optional<host::Mapping> surviving_view;
    {
        auto backing = host::SharedBacking::adopt_descriptor(descriptor, capacity);
        check(backing.size() == capacity, "shared backing adopts the explicit sealed bounded descriptor");
        host::Mapping first(3 * page, host::Access::none, page);
        host::Mapping second(2 * page, host::Access::none, page);
        backing.map_into(first, page, 0, page, host::Access::read_write);
        backing.map_into(second, 0, 0, page, host::Access::read_write);
        auto *original = first.data() + page;
        auto *alias = second.data();
        check(original[5] == std::byte{0} && alias[page - 1] == std::byte{0},
              "fresh shared backing is zero-filled at both ends of a guest-sized page");
        original[5] = std::byte{0x41};
        original[page - 1] = std::byte{0x5a};
        check(alias[5] == std::byte{0x41} && alias[page - 1] == std::byte{0x5a},
              "two owned reservations observe the same backing bytes without a copy");
        backing.map_into(second, 0, page, page, host::Access::read_write);
        alias[5] = std::byte{0xbb};
        check(original[5] == std::byte{0x41} && alias[page - 1] == std::byte{0},
              "replacing one alias with another backing offset leaves the prior page intact");
        first.decommit(page, page);
        backing.map_into(first, page, 0, page, host::Access::read);
        check(original[5] == std::byte{0x41} && original[page - 1] == std::byte{0x5a},
              "backing contents survive unmapping and remapping into the same reservation");
        const auto original_before_invalid = original[5];
        const auto alias_before_invalid = alias[5];
        check(runtime_rejected([&] { backing.map_into(first, 0, 1, page, host::Access::read_write); }) &&
                  runtime_rejected([&] { backing.map_into(first, 1, 0, page, host::Access::read_write); }) &&
                  runtime_rejected([&] { backing.map_into(first, 0, 0, 0, host::Access::read_write); }),
              "unaligned source/destination and empty backing views fail before replacing a mapping");
        check(
            runtime_rejected(
                [&] { backing.map_into(first, first.size(), 0, page, host::Access::read_write); }) &&
                runtime_rejected(
                    [&] { backing.map_into(first, 0, capacity, page, host::Access::read_write); }) &&
                runtime_rejected([&] { backing.map_into(first, 0, 0, SIZE_MAX, host::Access::read_write); }),
            "destination, backing and overflow bounds reject views outside owned ranges");
        check(runtime_rejected([&] { backing.map_into(first, 0, 0, page, host::Access::read_execute); }) &&
                  original[5] == original_before_invalid && alias[5] == alias_before_invalid,
              "executable backing views are unsupported and invalid operations preserve existing bytes");
        first.decommit(page, page);
        backing.discard(0, page);
        backing.map_into(first, page, 0, page, host::Access::read);
        check(original[5] == std::byte{0} && original[page - 1] == std::byte{0} &&
                  alias[5] == std::byte{0xbb},
              "discarded retired backing reads as zero without clearing adjacent live pages");
        check(runtime_rejected([&] { backing.discard(1, page); }) &&
                  runtime_rejected([&] { backing.discard(capacity, page); }),
              "discard validates alignment and bounds before touching the backing file");
        const auto duplicate = dup(descriptor);
        check(duplicate >= 3 && runtime_rejected([&] {
                  (void)host::SharedBacking::adopt_descriptor(duplicate, capacity - page);
              }) &&
                  fcntl(duplicate, F_GETFD) < 0 && errno == EBADF,
              "a rejected capacity closes its transferred descriptor without closing the valid owner");
        const auto ordinary = open("/work/ordinary-backing", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        check(ordinary >= 3 && runtime_rejected([&] {
                  (void)host::SharedBacking::adopt_descriptor(ordinary, capacity);
              }) &&
                  fcntl(ordinary, F_GETFD) < 0 && errno == EBADF,
              "an ordinary linked scratch file cannot masquerade as sealed anonymous backing");
        auto moved = std::move(backing);
        check(backing.size() == 0 && moved.size() == capacity &&
                  runtime_rejected([&] { backing.map_into(first, 0, 0, page, host::Access::read); }),
              "moving backing transfers its sole descriptor ownership and disables the old object");
        surviving_view.emplace(page, host::Access::none, page);
        moved.map_into(*surviving_view, 0, 2 * page, page, host::Access::read_write);
        surviving_view->data()[7] = std::byte{0x79};
    }
    check(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF && surviving_view->data()[7] == std::byte{0x79},
          "the backing owner closes its descriptor while an existing kernel view retains its data");
    surviving_view.reset();
#else
    (void)descriptor;
    throw std::runtime_error("shared backing fixtures require native Linux");
#endif
}
