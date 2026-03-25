```cmake
.\vcpkg.exe install nlohmann-json sqlitecpp spdlog croncpp tgbot-cpp yaml-cpp boost-process boost-asio cpp-httplib
cd cpp
# 假设已安装vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```