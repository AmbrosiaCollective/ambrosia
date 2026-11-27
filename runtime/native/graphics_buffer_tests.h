void graphics_buffer_checks(const std::filesystem::path &root) {
    using namespace ambrosia;
    host::Graphics graphics(root);
    check(runtime_rejected([&] { (void)graphics.create_buffer(65536); }),
          "buffer creation requires an owned native device");
    const auto device = graphics.create_device();
    check(runtime_rejected([&] { (void)graphics.create_buffer(0); }) &&
              runtime_rejected([&] { (void)graphics.create_buffer(256ull * 1024 * 1024 + 1); }),
          "empty and oversized buffers fail before service allocation");
    const auto first = graphics.create_buffer(131072);
    const auto second = graphics.create_buffer(65536);
    check(first.id && second.id > first.id && first.device_address && second.device_address &&
              first.bytes == 131072 && second.bytes == 65536,
          "owned real buffers have distinct increasing IDs and nonzero device addresses");
    std::vector<std::byte> data(32768), output(data.size());
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::byte>((index * 37 + 11) % 251);
    }
    graphics.download_buffer(first.id, 0, output);
    check(std::all_of(output.begin(), output.end(), [](auto value) { return value == std::byte{}; }),
          "new coherent buffer bytes are initially zero");
    graphics.upload_buffer(first.id, 0, data);
    graphics.download_buffer(first.id, 0, output);
    check(output == data, "directional file uploads and downloads preserve every byte");
    graphics.copy_buffer(first.id, 0, second.id, 0, static_cast<std::uint32_t>(data.size()));
    graphics.copy_buffer(second.id, 0, first.id, 98304, static_cast<std::uint32_t>(data.size()));
    graphics.download_buffer(first.id, 98304, output);
    check(output == data, "two chained real GPU copies preserve uploaded bytes through the file bridge");
    const std::array<std::byte, 3> small{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    std::array<std::byte, 3> small_output{};
    graphics.upload_buffer(first.id, 3, small);
    graphics.download_buffer(first.id, 3, small_output);
    check(small_output == small, "CPU byte transfers allow unaligned offsets independently of GPU copies");
    check(runtime_rejected([&] { graphics.download_buffer(999, 0, output); }) &&
              runtime_rejected([&] { graphics.upload_buffer(first.id, UINT64_MAX, data); }) &&
              runtime_rejected([&] { graphics.download_buffer(second.id, 65535, output); }) &&
              runtime_rejected([&] { graphics.upload_buffer(first.id, 0, {}); }),
          "foreign IDs, wrapping, out-of-range and empty chunks fail before transfer");
    check(runtime_rejected([&] { graphics.copy_buffer(first.id, 0, first.id, 4, 16); }) &&
              runtime_rejected([&] { graphics.copy_buffer(first.id, 1, second.id, 0, 4); }) &&
              runtime_rejected([&] { graphics.copy_buffer(first.id, 0, second.id, 0, 3); }),
          "overlapping and unaligned GPU copy ranges fail before submission");
    check(runtime_rejected([&] { graphics.destroy_device(device.id); }),
          "native device release cannot strand its live buffer ownership");
    graphics.destroy_buffer(first.id);
    check(runtime_rejected([&] { graphics.download_buffer(first.id, 0, output); }),
          "a released buffer cannot be downloaded again");
    const auto third = graphics.create_buffer(65536);
    check(third.id > second.id, "retired native buffer IDs are never reused");
    graphics.destroy_buffer(second.id);
    graphics.destroy_buffer(third.id);
    graphics.destroy_device(device.id);
    const auto recreated_device = graphics.create_device();
    const auto after_recreation = graphics.create_buffer(65536);
    check(after_recreation.id > third.id,
          "native buffer IDs remain monotonic across device recreation within one service session");
    graphics.destroy_buffer(after_recreation.id);
    graphics.destroy_device(recreated_device.id);
}
