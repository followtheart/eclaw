```cmake
.\vcpkg.exe install nlohmann-json sqlitecpp spdlog croncpp tgbot-cpp yaml-cpp boost-process boost-asio cpp-httplib
cd cpp
# 假设已安装vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

## Agent Runner 准备

项目不再依赖 Docker，直接在主机上运行 agent-runner：

```bash
cd container/agent-runner
npm install
npm run build
```

确保 `node` 在 PATH 中可用。
