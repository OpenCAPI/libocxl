ocxl_afp3, ocxl_afp3_latency
============================

`ocxl_afp3` and 'ocxl_afp3_latency' are test programs for the OpenCAPI AFU IBM,AFP3 (used for
development).

Requirements
------------

The OpenCAPI card must be flashed with an IBM,AFP3 AFU image.

Usage
-----

    $ ../../afuobj/ocxl_afp3

```
    Usage: ocxl_afp3 [ options ]
        --tags_ld     Number of tags for loads.  Default=0
        --tags_st     Number of tags for stores.  Default=0
                      0 -   0 tags (disabled)
                      1 -   1 tag
                      2 -   2 tags
                      3 -   4 tags
                      4 -  16 tags
                      5 -  64 tags
                      6 - 256 tags
                      7 - 512 tags
        --size_ld     Data size, in Bytes, for loads.
                      Supported values: 64, 128, 256.  Default=128
        --size_st     Data size, in Bytes, for stores.
                      Supported values: 64, 128, 256.  Default=128
        --npu_ld      Use rd_wnitc.n for loads.  Default is rd_wnitc
        --npu_st      Use dma_w.n for stores.  Default is dma_w
        --num         Number of times to check perf counts.  Default is 3
        --wait        Amount of seconds to wait between perf count reads.
                      Default is 2
        --prefetch    Initialize buffer memory
        --offsetmask  Determines how much of buffer to use.
                      Default 512kB.  Valid Range: 4K-512M.
                      Format: NumberLetter, e.g. 4K, 512K, 1M, 512M
	--device      Device to open instead of first AFP AFU found
        --verbose     Verbose output
        --help        Print this message
```


    $ ../../afuobj/ocxl_afp3_latency
    
```
    Usage: /home/fbarrat/capi/libocxl/afuobj/ocxl_afp3_latency [ options ]
        -i 10000     --iterations       Default=10000
        -p 64 to 512 --pong             Pong size from card to host (Bytes) Default=64
        -x           --extraread        Add an DMA extraread before the DMA Wr Default is no
        -f           --forever          Run until CTRL+C, Default=no
        -d           --device           Device to open instead of first AFP AFU found
        -v           --verbose          Verbose output
        -h           --help             Print this message
```
