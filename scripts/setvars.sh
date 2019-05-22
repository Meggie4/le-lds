#!/bin/bash
export LEVELDBSSDSRC=$PWD
export DBBENCH=$LEVELDBSSDSRC
export TEST_TMPDIR=/mnt/ssd
export TEST_MEMDIR=/mnt/pmemdir
#DRAM buffer size in MB
export DRAMBUFFSZ=4
#NVM buffer size in MB
