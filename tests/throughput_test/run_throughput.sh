#!/bin/bash

gb=3
sec=60
ncores_r=4
ncores_w=4

fsprefix="t1-"

numactl --cpunodebind=0 --membind=0 ./e.o $gb $ncores_r $sec > ./t1-rd &
numactl --cpunodebind=0 --membind=2 ./e.o $gb $ncores_r $sec > ./t1-rn &
numactl --cpunodebind=0 --membind=0 ./e_w.o $gb $ncores_w $sec > ./t1-wd &
numactl --cpunodebind=0 --membind=2 ./e_w.o $gb $ncores_w $sec > ./t1-wn
