CONFIG_MODULE_SIG=n
CC = gcc
CFLAGS = -Wall -lnuma -pthread -lm

MODULE_FILENAME=mod-placement
obj-m +=  $(MODULE_FILENAME).o
KO_FILE=$(MODULE_FILENAME).ko

export KROOT=/lib/modules/$(shell uname -r)/build

all: ctl client module

module:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules -j 12

module_install:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules_install -j 12

clean: 
	@$(MAKE) -C $(KROOT) M=$(PWD) clean
	rm -rf   Module.symvers modules.order *.o

insert:
	sudo insmod $(KO_FILE)

remove:
	sudo rmmod $(MODULE_FILENAME)

force-remove:
	sudo rmmod -f $(MODULE_FILENAME)

ctl: ctl-placement.c
	${CC} ${CFLAGS} -o ctl-placement.o ctl-placement.c

client: client.c client-placement.c
	${CC} ${CFLAGS} -o client.o client-placement.c client.c
