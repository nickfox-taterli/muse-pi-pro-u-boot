## 依赖

```bash
sudo apt-get update && sudo apt-get install -y \
  u-boot-tools device-tree-compiler \
  build-essential bc bison flex libssl-dev libelf-dev libncurses-dev \
  python3 swig lz4 lzop zlib1g-dev
```

## 工具链

```bash
TOOLCHAIN_URL="https://static.taterli.cyou/files/spacemit-toolchain-linux-glibc-x86_64-v1.1.2.tar.xz"
mkdir -p "$HOME/toolchains" && cd "$HOME/toolchains"
curl -fLO "$TOOLCHAIN_URL" || wget "$TOOLCHAIN_URL"
tar -xJf spacemit-toolchain-linux-glibc-x86_64-v1.1.2.tar.xz
export TOOLCHAIN_DIR="$HOME/toolchains/spacemit-toolchain-linux-glibc-x86_64-v1.1.2"
export PATH="$TOOLCHAIN_DIR/bin:$PATH"
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
```

## 构建(固定配置,不需要管其他配置.)

```bash
make k1-x_tiny_defconfig
make -j"$(nproc)" CROSS_COMPILE="${CROSS_COMPILE}"
```

## 确保编译是正确的!

* u-boot.itb 可用,大小没明显异常(常规产物 >200K && < 2MB)
* dumpimage -l u-boot.itb 可见RISC-V架构.
