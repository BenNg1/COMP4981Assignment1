set -e

cd "/cygdrive/c/BCIT/Term 4/COMP4981/Assignment1/COMP4981Assignment1/cmake-build-debug"
/cygdrive/c/Users/BenNguyen/AppData/Local/JetBrains/CLion2024.2/cygwin_cmake/bin/cmake.exe --regenerate-during-build -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR)
