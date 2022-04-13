#!/bin/bash

# BSD 2-Clause License
#
# Copyright (c) 2021-2022, Christoph Neuhauser, Felix Brendel
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

set -euo pipefail

SCRIPTPATH="$( cd "$(dirname "$0")" ; pwd -P )"
PROJECTPATH="$SCRIPTPATH"
pushd $SCRIPTPATH > /dev/null

debug=false
build_dir_debug=".build_debug"
build_dir_release=".build_release"
destination_dir="Shipping"

is_installed_brew() {
    local pkg_name="$1"
    if brew list $pkg_name > /dev/null; then
        return 0
    else
        return 1
    fi
}

if ! command -v brew &> /dev/null; then
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

if command -v brew &> /dev/null; then
    if ! is_installed_brew "git"; then
        brew install git
    fi
    if ! is_installed_brew "cmake"; then
        brew install cmake
    fi
    if ! is_installed_brew "pkg-config"; then
        brew install pkg-config
    fi
    if ! is_installed_brew "llvm"; then
        brew install llvm
    fi
    if ! is_installed_brew "libomp"; then
        brew install libomp
    fi

    # Homebrew MoltenVK does not contain script for setting environment variables, unfortunately.
    #if ! is_installed_brew "molten-vk"; then
    #    brew install molten-vk
    #fi
    if ! is_installed_brew "zlib"; then
        brew install zlib
    fi
    if ! is_installed_brew "libpng"; then
        brew install libpng
    fi
    if ! is_installed_brew "glm"; then
        brew install glm
    fi
    if ! is_installed_brew "sdl2"; then
        brew install sdl2
    fi
    if ! is_installed_brew "sdl2_image"; then
        brew install sdl2_image
    fi
    if ! is_installed_brew "libarchive"; then
        brew install libarchive
    fi
    if ! is_installed_brew "boost"; then
        brew install boost
    fi
    if ! is_installed_brew "tinyxml2"; then
        brew install tinyxml2
    fi

    if ! is_installed_brew "jsoncpp"; then
        brew install jsoncpp
    fi
    if ! is_installed_brew "openexr"; then
        brew install openexr
    fi
    if ! is_installed_brew "python@3.9"; then
        brew install python@3.9
    fi
fi

if ! command -v cmake &> /dev/null; then
    echo "CMake was not found, but is required to build the program."
    exit 1
fi
if ! command -v git &> /dev/null; then
    echo "git was not found, but is required to build the program."
    exit 1
fi

[ -d "./third_party/" ] || mkdir "./third_party/"
pushd third_party > /dev/null

if [ -z "${VULKAN_SDK+1}" ]; then
    echo "------------------------"
    echo "searching for Vulkan SDK"
    echo "------------------------"

    found_vulkan=false

    if [ -d "$HOME/VulkanSDK" ]; then
        source "$HOME/VulkanSDK/$(ls $HOME/VulkanSDK)/setup-env.sh"
        found_vulkan=true
    else
      VULKAN_SDK_VERSION=1.3.204.1
      curl -O https://sdk.lunarg.com/sdk/download/$VULKAN_SDK_VERSION/mac/vulkansdk-macos-$VULKAN_SDK_VERSION.dmg
      sudo hdiutil attach vulkansdk-macos-$VULKAN_SDK_VERSION.dmg
      sudo /Volumes/vulkansdk-macos-$VULKAN_SDK_VERSION/InstallVulkan.app/Contents/MacOS/InstallVulkan \
      --root ~/VulkanSDK/$VULKAN_SDK_VERSION --accept-licenses --default-answer --confirm-command install
      pushd ~/VulkanSDK/$VULKAN_SDK_VERSION
      sudo python3 ./install_vulkan.py
      popd
      sudo hdiutil unmount /Volumes/vulkansdk-macos-$VULKAN_SDK_VERSION
      source "$HOME/VulkanSDK/$(ls $HOME/VulkanSDK)/setup-env.sh"
      found_vulkan=true
    fi

    if ! $found_vulkan; then
        echo "The environment variable VULKAN_SDK is not set but is required in the installation process."
        echo "Please refer to https://vulkan.lunarg.com/sdk/home#mac for instructions on how to install the Vulkan SDK."
        exit 1
    fi
fi

if [ ! -d "./sgl" ]; then
    echo "------------------------"
    echo "     fetching sgl       "
    echo "------------------------"
    git clone --depth 1 https://github.com/chrismile/sgl.git
fi

if [ ! -d "./sgl/install" ]; then
    echo "------------------------"
    echo "     building sgl       "
    echo "------------------------"

    pushd "./sgl" >/dev/null
    mkdir -p .build_debug
    mkdir -p .build_release

    pushd "$build_dir_debug" >/dev/null
    cmake .. \
         -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=False -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=False \
         -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_FIND_APPBUNDLE=NEVER -DZLIB_ROOT="/usr/local/opt/zlib" \
         -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DCMAKE_INSTALL_PREFIX="../install"
    popd >/dev/null

    pushd $build_dir_release >/dev/null
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=False -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=False \
        -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_FIND_APPBUNDLE=NEVER -DZLIB_ROOT="/usr/local/opt/zlib" \
        -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DCMAKE_INSTALL_PREFIX="../install"
    popd >/dev/null

    cmake --build $build_dir_debug --parallel
    cmake --build $build_dir_debug --target install

    cmake --build $build_dir_release --parallel
    cmake --build $build_dir_release --target install

    popd >/dev/null
fi

popd >/dev/null # back to project root

if [ $debug = true ] ; then
    echo "------------------------"
    echo "  building in debug     "
    echo "------------------------"

    cmake_config="Debug"
    build_dir=$build_dir_debug
else
    echo "------------------------"
    echo "  building in release   "
    echo "------------------------"

    cmake_config="Release"
    build_dir=$build_dir_release
fi
mkdir -p $build_dir

echo "------------------------"
echo "      generating        "
echo "------------------------"
pushd $build_dir >/dev/null
cmake -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=False -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=False \
      -DCMAKE_FIND_FRAMEWORK=LAST -DCMAKE_FIND_APPBUNDLE=NEVER -DZLIB_ROOT="/usr/local/opt/zlib" \
      -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
      -DCMAKE_BUILD_TYPE=$cmake_config \
      -Dsgl_DIR="$PROJECTPATH/third_party/sgl/install/lib/cmake/sgl/" ..
popd >/dev/null

echo "------------------------"
echo "      compiling         "
echo "------------------------"
cmake --build $build_dir --parallel

echo "------------------------"
echo "   copying new files    "
echo "------------------------"

[ -d $destination_dir ]             || mkdir $destination_dir

rsync -a $build_dir/CloudRendering $destination_dir
rsync -a "$VULKAN_SDK/lib/libMoltenVK.dylib" $destination_dir

echo ""
echo "All done!"


pushd $build_dir >/dev/null

# https://stackoverflow.com/questions/2829613/how-do-you-tell-if-a-string-contains-another-string-in-posix-sh
contains() {
    string="$1"
    substring="$2"
    if test "${string#*$substring}" != "$string"
    then
        return 0
    else
        return 1
    fi
}

if [ -z "${DYLD_LIBRARY_PATH+x}" ]; then
    export DYLD_LIBRARY_PATH="${PROJECTPATH}/third_party/sgl/install/lib"
elif contains "${DYLD_LIBRARY_PATH}" "${PROJECTPATH}/third_party/sgl/install/lib"; then
    export DYLD_LIBRARY_PATH="DYLD_LIBRARY_PATH:${PROJECTPATH}/third_party/sgl/install/lib"
fi
export DYLD_LIBRARY_PATH="DYLD_LIBRARY_PATH:$destination_dir"
./CloudRendering
