obj-m = mars.o

K_DIR ?= /lib/modules/$(shell uname -r)/build
CC ?= gcc 
LD ?= ld
ARCH ?= x86_64

all:
	make ARCH=$(ARCH) -C $(K_DIR) M=$(PWD) modules
clean:
	make ARCH=$(ARCH) -C $(K_DIR) M=$(PWD) clean
