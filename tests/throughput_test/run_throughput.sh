#!/bin/bash

gb=64
sec=60
ncores_r=4
ncores_w=4

fsprefix="pnp"

numactl --cpunodebind=0 ./e.o $gb $ncores_r $sec > ./${fsprefix}-rd &
