ocxl_reset_tests.sh
===================

`ocxl_reset_tests.sh` is a script for testing the reset of an OpenCAPI card.

Requirements
------------

The OpenCAPI card must be flashed with either an IBM,AFP3 or IBM,MEMCPY3 AFU
image.

This test requires the kernel module pnv-php, that will be automatically
loaded.

Usage
-----

    $ ../../afuobj/ocxl_reset_tests.sh  # Reset the first card and check AFU

```
    Usage: ocxl_reset_tests [ options ]
    Options:
        -d <device>   Use this capi card
        -l <loops>    Run this number of resets (default 1)
```
