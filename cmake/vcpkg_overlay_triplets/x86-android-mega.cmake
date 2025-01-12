set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=i686-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=x86 -DANDROID_PLATFORM=android-26 -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON)
