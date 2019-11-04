This repository contains multiple wait-free Persistent Transactional Memories (PTM).
<p>
To build the benchmarks, go into the graphs/ folder and type make.
If you have PMDK installed, you can do 'make pmdk' to have comparisons with libpmemobj.
<p>
By default it uses /dev/shm/ as a persistent memory region (2GB). If you have Intel Optane DC PM then edit the Makefile, enable CLWBs, set the file for the mmaped region (should be an ext4 filesystem with DAX enabled, typically /mnt/pmem0/ ).