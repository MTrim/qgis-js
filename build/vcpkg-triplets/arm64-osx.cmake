set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

# Host-Tools auf einem Mac ohne Xcode.app (nur Command Line Tools): Qt prueft
# sonst die Xcode-Version und bricht bei qttools ab ("Can't determine Xcode
# version"). Fuer die Host-Werkzeuge (moc, rcc, lrelease ...) unerheblich.
if(PORT MATCHES "^qt")
  set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DQT_NO_APPLE_SDK_AND_XCODE_CHECK=ON)
endif()
