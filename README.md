VERCL
=====

Things to change to make it runnable on any 2-node setup:

* `ETH_NAME` => should match the NIC name you want to use
* remote ips (first parameter in `test-rt-*.cc` files)
* `/proc/sys/net/core/{w,r}mem_{default,max}` should increased (code checks for at least 1MB buffers?)

```
$ ssh Node1
$ cd path-to/build
$ ./test-rt-loopback

$ ssh Node2
$ cd path-to/build
$ ./test-rt-sender
```

Example output:
---------------

```
root@AMTHost3:/home/mueller/vercl# build/test/test-rt-loopback 
using local ip 10.0.0.3
remote ip 10.0.0.2 mac 00:12:55:03:51:c8
looping back spike -> label: 1

root@AMTHost2:/home/mueller/vercl# build/test/test-rt-sender 
using local ip 10.0.0.2
remote ip 10.0.0.3 mac 00:12:55:03:51:c4
```


The current code needs root permissions due to raw socket operations and
mmapping of socket/kernel tx/rx buffers.
