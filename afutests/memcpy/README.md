ocxl_memcpy
===========

`ocxl_memcpy` is a test program for the OpenCAPI AFU IBM,MEMCPY3 (used for
development).

Requirements
------------

The OpenCAPI card must be flashed with an IBM,MEMCPY3 AFU image.

Usage
-----

    $ ../../afuobj/ocxl_memcpy     # Test memcpy AFU memory copy
    $ ../../afuobj/ocxl_memcpy -A  # Test memcpy AFU atomic compare and swap
    $ ../../afuobj/ocxl_memcpy -a  # Test memcpy AFU increment

```
    Usage: ocxl_memcpy [ options ]
    Options:
        -A            Run the atomic compare and swap test
        -a            Run the increment test
        -d <device>   Use this capi card
        -I            Initialize the destination buffer after each loop
        -i            Send an interrupt after copy
        -l <loops>    Run this number of memcpy loops (default 1)
        -p <procs>    Fork this number of processes (default 1)
        -p 0          Use the maximum number of processes permitted by the AFU
        -r            Reallocate the destination buffer in between 2 loops
        -S            Operate on shared memory
        -s <bufsize>  Copy this number of bytes (default 2048)
        -t <timeout>  Seconds to wait for the AFU to signal completion
```
