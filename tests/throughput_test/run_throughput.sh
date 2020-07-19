#!/bin/bash

gb=3
sec=60
ncores_r=8
ncores_w=8

numactl --cpunodebind=0 --membind=0 ./e.o $gb $ncores_r $sec &
numactl --cpunodebind=0 --membind=2 ./e.o $gb $ncores_r $sec &
numactl --cpunodebind=0 --membind=0 ./e_w.o $gb $ncores_w $sec &
numactl --cpunodebind=0 --membind=2 ./e_w.o $gb $ncores_w $sec
