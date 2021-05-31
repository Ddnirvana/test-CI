#!/bin/bash
cd /home/penglai/penglai-enclave/opensbi-0.9
mkdir -p build-oe/qemu-virt
CROSS_COMPILE=riscv64-unknown-linux-gnu- make O=build-oe/qemu-virt PLATFORM=generic FW_PAYLOAD=y FW_PAYLOAD_PATH=/home/penglai/penglai-enclave/Image
