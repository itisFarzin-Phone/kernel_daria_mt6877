rm -rf AnyKernel3 KernelSU* drivers/kernelsu* *.zip

if [ ! -d clang ]; then
    wget https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/tags/android-15.0.0_r15/clang-r536225.tar.gz -O clang.tar.gz
    mkdir clang
    tar -xf clang.tar.gz -C clang
fi

export PATH="$(pwd)/clang/bin:$PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib64:$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="$(pwd)/clang/lib:$LD_LIBRARY_PATH"
export ARCH=arm64
export SUBARCH=ARM64
export KBUILD_BUILD_USER="itisFarzin"
export KBUILD_BUILD_HOST="itisFarzin"

zip_name="$(git rev-parse --abbrev-ref HEAD)-mt6877-$(date +'%Y%m%d').zip"

curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next/kernel/setup.sh" | bash -

make CC=clang O=out clean mrproper k6877v1_64_k419_defconfig LLVM=1 LLVM_IAS=1
make CC=clang O=out -j$(nproc --all) LLVM=1 LLVM_IAS=1

git clone --depth=1 https://github.com/itisFarzin-Phone/AnyKernel3 -b zahedan
cp out/arch/arm64/boot/Image.gz-dtb AnyKernel3/

if [ "$1" = "--vendor_dlkm" ]; then
    if [ -f /mnt/Android/LineageOS/out/target/product/zahedan/vendor_dlkm.img ]; then
        cp /mnt/Android/LineageOS/out/target/product/zahedan/vendor_dlkm.img AnyKernel3/
    elif [ -f vendor_dlkm.img ]; then
        cp vendor_dlkm.img AnyKernel3/
    else
        echo "ERROR: vendor_dlkm.img doesn't exist!"
        exit 1
    fi
fi

cd AnyKernel3/
zip -r9 "../$zip_name" * -x .git README.md *placeholder
