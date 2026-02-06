sudo cp -f arch/arm64/boot/Image.gz /mnt/alioth_boot/vmlinuz_linux
sudo cp -f arch/arm64/boot/dts/qcom/sm8250-xiaomi-alioth.dtb /mnt/alioth_boot/sm8250-xiaomi-alioth.dtb
sudo make ARCH=arm64 INSTALL_MOD_PATH=/mnt/alioth_root modules_install
