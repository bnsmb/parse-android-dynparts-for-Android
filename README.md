The binary created with the source files from this repository creates input files for the Android binary **dmctl**.

**dmctl** can only use input files with the configuration for one logical device. Therefore, this tool creates separate input files for each logical device..

The usage for the tool is:

```
ASUS_I006D:/data/local/tmp/develop/parse-android-dynparts/build # ./parse-android-dynparts                                                                                                                                        
Usage: ./parse-android-dynparts [OPTIONS] <super_device>
Options:
  -s, --slot N       Slot number (0 or 1) [default: current slot]
  -o, --outdir DIR   Output directory for dmctl config files [default: .]
  -p, --prefix PREFIX
                     Prefix to add to logical device names
  -r, --rw           Create devices read-write (omit -ro flag)
      --skip-cow     Ignore -cow partitions (cannot be mounted)
  -h, --help         Show this help

Creates one file per logical partition: <outdir>/dmctl_<name>.txt
Usage: dmctl -f <file>
1|ASUS_I006D:/data/local/tmp/develop/parse-android-dynparts/build # 
```

The source code can be compiled in Android using the **clang19 toolchain** for Android. The **clang19 toolchain** for Android is available here:

[https://github.com/bnsmb/clang19_toolchain_for_android](https://github.com/bnsmb/clang19_toolchain_for_android)

Use 
```
git clone git@github.com:bnsmb/parse-android-dynparts-for-Android.git
```
to copy the source files to the phone.

To compile the binary for Android, execute the instructions using **cmake** and **ninja** listed below 

Then execute the script **compile_parse.sh** when in the directory **./build** to create an executable that can be used while the phone is booted into the recovery:
```
ASUS_I006D:/data/local/tmp/develop/parse-android-dynparts/build # ldd /data/local/tmp/develop/parse-android-dynparts/build/parse-android-dynparts                                                                                 
	linux-vdso.so.1 => [vdso] (0x70d3770000)
	libc.so => /apex/com.android.runtime/lib64/bionic/libc.so (0x70ce297000)
	libm.so => /apex/com.android.runtime/lib64/bionic/libm.so (0x70d0101000)
	libdl.so => /apex/com.android.runtime/lib64/bionic/libdl.so (0x70d00f3000)
ASUS_I006D:/data/local/tmp/develop/parse-android-dynparts/build #
```
----

Purpose
=======
Most devices running Android 10 and higher use Android's [Dynamic Partitions][1]
feature to allow the different read-only system partitions (e.g. `system`,
`vendor`, `product`) to share the same pool of storage space. This allows
vendors to safely resize those partitions in OTA updates, as long as the sum of
their sizes doesn't exceed that of the physical partition they all reside in.

The physical partition image that holds multiple Android dynamic partitions is
conventionally named `super.img` and holds similar information as an LVM
physical volume on Linux: a list of logical partitions, each associated with a
(possibly non-contiguous) set of blocks in the file that comprise it. Like LVM,
Android makes use of [Device Mapper's dm-linear target][2] to inform the
kernel of the logical partitions so it can map them to block devices in
`/dev/mapper`.

In true Google fashion, however, Android dynamic partitions use a totally custom
header format that is not compatible with LVM or other similar software. As
such, the only official tools that exist to mount them are part of Android and
depend heavily on Android's frameworks, volume manager, and init system. (There
are [official tools][3] that run on Linux to pack and unpack `super.img` files,
but they cannot mount them in-place.)

This tool makes it possible to mount `super.img` files with a standard Linux
userspace. It uses a modified version of Google's AOSP code to parse the
partition layout, then outputs that layout as a textual "concise device
specification" which, when passed to `dmsetup`, instructs the kernel to create
a Device Mapper block device for each logical partition in the image.

[1]: https://source.android.com/devices/tech/ota/dynamic_partitions
[2]: https://www.kernel.org/doc/html/latest/admin-guide/device-mapper/linear.html
[3]: https://android.googlesource.com/platform/system/extras/+/master/partition_tools/

Dependencies
============
 - CMake
 - OpenSSL (for hash functions)

Building
========
This is a standard C++ CMake project; it builds like any other CMake project.
For those unfamiliar with CMake, here's the incantation you need to build using
[Ninja](https://ninja-build.org/) as a backend:
```
mkdir build
cd build
cmake -G Ninja ..
ninja
```

Or, if you don't have Ninja, you can use the Makefile backend:
```
mkdir build
cd build
cmake ..
make
```

Usage
=====

Setup
-----
 1. Obtain a raw `super.img`. Depending on the source of the image you're
    working with, this may initially be a sparse image, which you'll have to
    unsparse using the standard Android `simg2img` tool, or it may be one
    partition inside a GPT-partitioned disk image.
 2. Make your `super.img` available as a loop device (omit `-r` if you want to
    allow writes):
    ```
    losetup -r /dev/loop0 super.img
    ```
 3. Create mappings for the dynamic partitions:
    ```
    dmsetup create --concise "$(parse-android-dynparts /dev/loop0)"
    ```
 4. Access your partitions as `/dev/mapper/dynpart-<NAME>`!

Teardown
--------
 1. Unmap the Device Mapper devices:
    ```
    dmsetup remove /dev/mapper/dynpart-*
    ```
 2. Delete the loop device:
    ```
    losetup -d /dev/loop0
    ```
