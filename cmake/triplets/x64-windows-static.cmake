# Custom x64-windows-static triplet for Locus
# Disables MSVC STL vector SIMD intrinsics that are absent from libcpmt.lib
# in VS 2022 17.7+ when linking statically.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CXX_FLAGS "/D_USE_STD_VECTOR_ALGORITHMS=0")
set(VCPKG_C_FLAGS "/D_USE_STD_VECTOR_ALGORITHMS=0")
