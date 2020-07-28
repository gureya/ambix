CONFIG_MODULE_SIG=n
CC = gcc
CFLAGS = -Wall -lnuma -pthread -lm

MODULE_FILENAME=pnp_hyb-mod
obj-m +=  $(MODULE_FILENAME).o
KO_FILE=$(MODULE_FILENAME).ko

export KROOT=/lib/modules/$(shell uname -r)/build

all: ctl client module

module: pnp_hyb-mod.c pnp.h
	@$(MAKE) -C $(KROOT) M=$(PWD) modules -j 12

module_install:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules_install -j 12

clean:
	@$(MAKE) -C $(KROOT) M=$(PWD) clean
	rm -rf   Module.symvers modules.order *.o *.mod socket

insert:
	sudo insmod $(KO_FILE)

remove:
	sudo rmmod $(MODULE_FILENAME)

force-remove:
	sudo rmmod -f $(MODULE_FILENAME)

ctl: pnp_hyb-ctl.c pnp.h
	${CC} ${CFLAGS} -o pnp_hyb-ctl.o pnp_hyb-ctl.c

client: client.c client_2.c pnp-client.c pnp.h
	${CC} ${CFLAGS} -o client.o pnp-client.c client.c
	${CC} ${CFLAGS} -o client_2.o pnp-client.c client_2.c
