# 用户空间程序构建配置
CC = clang
OBJCOPY = llvm-objcopy

# 编译标志
CFLAGS = -target aarch64-linux-gnu \
         -ffreestanding \
         -fno-stack-protector \
         -nostdlib \
         -nostartfiles \
         -fPIC \
         -O2 \
         -Wall \
         -Wextra

# 链接标志
LDFLAGS = -target aarch64-linux-gnu \
          -nostdlib \
          -static \
          -Wl,-T,userspace.ld

# 目标文件
TARGETS = hello.elf hello.bin hello.o

all: $(TARGETS)

# 编译目标文件
hello.o: hello.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 链接ELF可执行文件
hello.elf: hello.o
	$(CC) $(LDFLAGS) -o $@ $<

# 生成二进制镜像
hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

# 生成十六进制数组（用于嵌入内核）
hello.h: hello.bin
	xxd -i hello.bin > $@

# 查看ELF信息
info: hello.elf
	file hello.elf
	readelf -h hello.elf
	readelf -l hello.elf
	objdump -d hello.elf | head -20

# 清理
clean:
	rm -f *.o *.elf *.bin *.h

.PHONY: all info clean