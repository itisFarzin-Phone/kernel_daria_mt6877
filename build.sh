rm -rf AnyKernel3
rm -f FlashableKernel-zahedan-*-KSU.zip

git clone https://github.com/itisFarzin/android_prebuilts_clang_host_linux-x86_clang-r416183b ./clang --depth=1

export PATH="$(pwd)/clang/bin:$PATH"
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

androidversion=$1
if [ -z "$androidversion" ]; then
    echo "No Andorid version provided, setting it to 14"
    androidversion=14
fi

if [ $androidversion != "13" ] && [ $androidversion != "14" ]; then
    echo "Error: Andorid version must be either 13 or 14"
    exit 1
fi

if [ -d "KernelSU" ]; then
  rm -rf "KernelSU"
fi
if [ -d "drivers/kernelsu" ]; then
  rm -rf "drivers/kernelsu"
fi

curl -LSs "https://raw.githubusercontent.com/rsuntk/KernelSU/main/kernel/setup.sh" | bash -

make O=out CC=clang k6877v1_64_k419_defconfig LLVM=1 LLVM_IAS=1 LD=ld.lld HOSTLD=ld.lld HOSTLDFLAGS=-fuse-ld=lld
make O=out CC=clang -j$(nproc --all) HOSTCC=$(pwd)/clang/bin/clang HOSTCXX=$(pwd)/clang/bin/clang++ LD=$(pwd)/clang/bin/ld.lld AR=$(pwd)/clang/bin/llvm-ar LLVM=1 LLVM_IAS=1 CC=" clang --cuda-path=/dev/null"

git clone --recursive --depth=1 https://github.com/itisFarzin-Phone/AnyKernel3 -b zahedan AnyKernel3
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3
sed -i "s/supported\.versions=/supported\.versions=$androidversion/" AnyKernel3/anykernel.sh
cd AnyKernel3
zip -q -r "../FlashableKernel-zahedan-$today-A$androidversion-KSU.zip" *
