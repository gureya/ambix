CONFIG_MODULE_SIG=n

MODULE_FILENAME=dirty
obj-m +=  $(MODULE_FILENAME).o
KO_FILE=$(MODULE_FILENAME).ko

export KROOT=/lib/modules/$(shell uname -r)/build

modules:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules -j 12

modules_install:
	@$(MAKE) -C $(KROOT) M=$(PWD) modules_install -j 12

clean: 
	@$(MAKE) -C $(KROOT) M=$(PWD) clean
	rm -rf   Module.symvers modules.order

insert:
	sudo insmod $(KO_FILE)

remove:
	sudo rmmod $(MODULE_FILENAME)

printlog:
	sudo dmesg -c 
	sudo insmod $(KO_FILE)
	dmesg