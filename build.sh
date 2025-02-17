rm -rf KernelSU* drivers/kernelsu*
rm -f FlashableKernel-zahedan-*.zip

git clone https://github.com/itisFarzin-Phone/android_prebuilts_clang_host_linux-x86_clang-r416183b clang --depth=1
git clone https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 -b android-msm-redbull-4.19-android14 --depth=1
git clone https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9 -b android-msm-redbull-4.19-android14 --depth=1

export PATH="$(pwd)/clang/bin:$PATH"
export PATH="$(pwd)/aarch64-linux-android-4.9/bin:$PATH"
export PATH="$(pwd)/arm-linux-androideabi-4.9/bin:$PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib64:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib:$LD_LIBRARY_PATH"
export ARCH=arm64
export SUBARCH=ARM64
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-androideabi-
export KBUILD_BUILD_USER="itisFarzin"
export KBUILD_BUILD_HOST="itisFarzin"

today=$(date +"%Y%m%d")

curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next/kernel/setup.sh" | bash -

make O=out CC=clang k6877v1_64_k419_defconfig LLVM=1 LLVM_IAS=1 LD=ld.lld HOSTLD=ld.lld HOSTLDFLAGS=-fuse-ld=lld
make O=out CC=clang -j$(nproc --all) HOSTCC=$(pwd)/clang/bin/clang HOSTCXX=$(pwd)/clang/bin/clang++ LD=$(pwd)/clang/bin/ld.lld AR=$(pwd)/clang/bin/llvm-ar LLVM=1 LLVM_IAS=1 CC=" clang --cuda-path=/dev/null"

git clone --depth=1 https://github.com/itisFarzin-Phone/AnyKernel3 -b zahedan
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3/
cd AnyKernel3/
zip -q -r "../FlashableKernel-zahedan-$today.zip" *
