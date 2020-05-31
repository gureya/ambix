CONFIG_MODULE_SIG=n
CC = gcc
CFLAGS = -Wall -lnuma -pthread -lm

MODULE_FILENAME=clockdwf-mod
obj-m +=  $(MODULE_FILENAME).o
KO_FILE=$(MODULE_FILENAME).ko

export KROOT=/lib/modules/$(shell uname -r)/build

all: ctl client module

module: clockdwf-mod.c clockdwf.h
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

ctl: clockdwf-ctl.c clockdwf.h
	${CC} ${CFLAGS} -o clockdwf-ctl.o clockdwf-ctl.c

client: client.c client_2.c clockdwf-client.c clockdwf.h
	${CC} ${CFLAGS} -o client.o clockdwf-client.c client.c
	${CC} ${CFLAGS} -o client_2.o clockdwf-client.c client_2.c
