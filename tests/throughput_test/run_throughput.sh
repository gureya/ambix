#!/bin/bash

gb=3
sec=60
ncores_r=4
ncores_w=4

fsprefix="t1"

numactl --cpunodebind=0 --membind=0 ./e.o $gb $ncores_r $sec > ./${fsprefix}-rd &
numactl --cpunodebind=0 --membind=2 ./e.o $gb $ncores_r $sec > ./${fsprefix}-rn &
numactl --cpunodebind=0 --membind=0 ./e_w.o $gb $ncores_w $sec > ./${fsprefix}-wd &
numactl --cpunodebind=0 --membind=2 ./e_w.o $gb $ncores_w $sec > ./${fsprefix}-wn

