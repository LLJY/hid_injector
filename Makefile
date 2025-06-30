obj-m := hid_injector_v2.o

CFLAGS_REMOVE_hid_injector.o = -fmin-function-alignment=4

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

load: module
	sudo insmod ./hid_injector_v2.o

unload:
	sudo rmmod hid_injector || true