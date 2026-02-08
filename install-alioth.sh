sudo cp -f arch/arm64/boot/Image.gz ../alioth-6.19-exp3/vmlinuz_linux
sudo cp -f arch/arm64/boot/dts/qcom/sm8250-xiaomi-alioth.dtb ../alioth-6.19-exp3/sm8250-xiaomi-alioth.dtb
sudo make ARCH=arm64 INSTALL_MOD_PATH=../alioth-6.19-exp3 modules_install
