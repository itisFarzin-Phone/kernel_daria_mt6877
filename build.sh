rm -rf AnyKernel3 KernelSU* drivers/kernelsu* *.zip

git clone https://github.com/itisFarzin-Phone/android_prebuilts_clang_host_linux-x86_clang-r416183b clang --depth=1
git clone https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 -b android-msm-redbull-4.19-android14 --depth=1

export PATH="$(pwd)/clang/bin:$PATH"
export PATH="$(pwd)/aarch64-linux-android-4.9/bin:$PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib64:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib:$LD_LIBRARY_PATH"
export ARCH=arm64
export SUBARCH=ARM64
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER="itisFarzin"
export KBUILD_BUILD_HOST="itisFarzin"

zip_name="$(git rev-parse --abbrev-ref HEAD)-mt6877-$(date +'%Y%m%d').zip"

curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next/kernel/setup.sh" | bash -

make O=out CC=clang k6877v1_64_k419_defconfig LLVM=1 LLVM_IAS=1
make O=out CC=clang -j$(nproc --all) LLVM=1 LLVM_IAS=1

git clone --depth=1 https://github.com/itisFarzin-Phone/AnyKernel3 -b zahedan
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3/

if [ "$1" = "--vendor_dlkm" ]; then
    if [ -f vendor_dlkm.img ]; then
        cp vendor_dlkm.img AnyKernel3/
    elif [ -f /mnt/Android/LineageOS/out/target/product/zahedan/vendor_dlkm.img ]; then
        cp /mnt/Android/LineageOS/out/target/product/zahedan/vendor_dlkm.img AnyKernel3/
    else
        echo "ERROR: vendor_dlkm.img doesn't exist!"
        exit 1
    fi
fi

cd AnyKernel3/
zip -r9 "../$zip_name" * -x .git README.md *placeholder
