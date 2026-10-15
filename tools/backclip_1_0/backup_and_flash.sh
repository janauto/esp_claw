#!/bin/zsh
# 磁吸背夹 1.0：刷写前分区备份 + 只写应用分区（0x20000）与 SYSTEM（0xa20000）。
# 不写 bootloader、分区表、otadata、NVS、storage 和 SD 卡。
# 用法：
#   backup_and_flash.sh backup <备份目录>          # 读回 app/system/nvs/otadata 并在设备上校验
#   backup_and_flash.sh app <edge_agent.bin>        # 写应用分区并校验
#   backup_and_flash.sh system <system.bin>         # 写 SYSTEM 分区并校验
set -u
PY=~/esp/tools-mosaico-v6.1/python_env/idf6.1_py3.11_env/bin/python
PORT=${BACKCLIP_PORT:-/dev/cu.usbserial-10}
ES=($PY -m esptool --chip esp32s31 -p $PORT -b 460800 --before default-reset --no-stub)
lsof $PORT >/dev/null 2>&1 && { echo "串口 $PORT 被占用，停止"; exit 2; }

read_region() {   # name offset size outdir
  local name=$1 off=$2 size=$3 out=$4
  for try in 1 2 3; do
    if $ES --after no-reset read-flash $off $size $out/$name.bin >/dev/null 2>&1 && \
       [ $(stat -f%z $out/$name.bin) -eq $(( size )) ]; then
      if $ES --after no-reset verify-flash $off $out/$name.bin >/dev/null 2>&1; then
        echo "  $name ($off, $(( size )) 字节) 读回并校验通过 $(shasum -a 256 $out/$name.bin | cut -c1-16)…"
        return 0
      fi
    fi
    echo "  $name 第 $try 次失败，重试"; sleep 2
  done
  echo "  $name 备份失败"; return 1
}

case ${1:-} in
  backup)
    OUT=$2; mkdir -p $OUT
    echo "== $(date '+%T') 备份到 $OUT"
    read_region otadata 0x10000 0x2000 $OUT || exit 3
    read_region nvs 0xa000 0x6000 $OUT || exit 3
    read_region app_ota0 0x20000 0x500000 $OUT || exit 3
    read_region system 0xa20000 0x28a000 $OUT || exit 3
    shasum -a 256 $OUT/*.bin > $OUT/SHA256SUMS
    echo "== $(date '+%T') 备份完成"
    ;;
  app)
    BIN=$2
    echo "== $(date '+%T') 写应用分区 0x20000：$BIN（$(stat -f%z $BIN) 字节）"
    [ $(stat -f%z $BIN) -le $(( 0x500000 )) ] || { echo "镜像超过 5 MiB，停止"; exit 4; }
    $ES --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0x20000 $BIN 2>&1 | grep -vE "^Writing at|━" || true
    ;;
  system)
    BIN=$2
    echo "== $(date '+%T') 写 SYSTEM 分区 0xa20000：$BIN（$(stat -f%z $BIN) 字节）"
    [ $(stat -f%z $BIN) -le $(( 0x28a000 )) ] || { echo "镜像超过 SYSTEM 分区，停止"; exit 4; }
    $ES --after hard-reset write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m 0xa20000 $BIN 2>&1 | grep -vE "^Writing at|━" || true
    ;;
  *)
    echo "用法：$0 backup <目录> | app <bin> | system <bin>"; exit 1;;
esac
