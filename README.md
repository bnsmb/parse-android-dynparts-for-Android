
**CAUTION: This repository is still work in progress**

Purpose
=======

The source files in this repository can be used to create the Android program **mount_dynamic_partitions**.

**mount_dynamic_partitions** reads the information from a super partition on a phone running the Android OS and creates input files for **dmctl** to create logical devices for the dynamic partitions in the super partition.

The usage for the program is:

```
ASUS_I006D:/ $ mount_dynamic_partitions -h                                
Usage: mount_dynamic_partitions [OPTIONS] [<super_device>]
Options:
  -s, --slot N          Slot number (0 or 1) [default: current slot]
  -o, --outdir DIR      Output directory for dmctl config files [default: .]
  -p, --prefix PREFIX   Prefix to add to logical device names
  -r, --rw              Create devices read-write (omit -ro flag)
      --skip-cow        Ignore -cow partitions
      --partitions LIST Comma-separated list of partition names
  -x, --execute         Execute dmctl for each config file
      --delete          Delete config file after successful execution
      --keep            Keep config file (default)
      --mountdir DIR    Mount devices under DIR/<partname>
      --list            List partitions in selected slot
      --list-all        List partitions in all slots
  -h, --help            Show this help

Default super device: /dev/block/by-name/super
Environment DMCTL overrides dmctl path.
ASUS_I006D:/ $ 
```

<details><summary><b>Example</b></summary>
	<br>
<samp>
ASUS_I006D:/ # mkdir -p /data/local/tmp/rw_mounted_partitions <br>
ASUS_I006D:/ # 	<br>
<br>
ASUS_I006D:/ # mount_dynamic_partitions --prefix rw_ --outdir /data/local/tmp/rw_mounted_partitions --mountdir /data/local/tmp/rw_mounted_partitions --skip-cow --keep --rw<br>                 
Auto-detected slot 1<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_lukspart001.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_lukspart001.txt<br>
mount: /dev/block/mapper/rw_lukspart001: need -t<br>
No filesystem on rw_lukspart001, skipping mount.<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_odm_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_odm_b.txt<br>
Mounted /dev/block/mapper/rw_odm_b on /data/local/tmp/rw_mounted_partitions/odm_b (rw)<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_product_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_product_b.txt<br>
Mounted /dev/block/mapper/rw_product_b on /data/local/tmp/rw_mounted_partitions/product_b (rw)<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_system_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_system_b.txt<br>
Mounted /dev/block/mapper/rw_system_b on /data/local/tmp/rw_mounted_partitions/system_b (rw)<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_system_ext_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_system_ext_b.txt<br>
Mounted /dev/block/mapper/rw_system_ext_b on /data/local/tmp/rw_mounted_partitions/system_ext_b (rw)<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_vendor_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_vendor_b.txt<br>
Mounted /dev/block/mapper/rw_vendor_b on /data/local/tmp/rw_mounted_partitions/vendor_b (rw)<br>
<br>
Created: /data/local/tmp/rw_mounted_partitions/dmctl_vendor_dlkm_b.txt<br>
Executing: dmctl -f /data/local/tmp/rw_mounted_partitions/dmctl_vendor_dlkm_b.txt<br>
Mounted /dev/block/mapper/rw_vendor_dlkm_b on /data/local/tmp/rw_mounted_partitions/vendor_dlkm_b (rw)<br>
<br>
Execution results: 6 succeeded, 1 failed.<br>
1|ASUS_I006D:/ # <br>
	<br>
1|ASUS_I006D:/ # mount | grep  rw_<br>
/dev/block/dm-13 on /data/local/tmp/rw_mounted_partitions/odm_b type ext4 (rw,seclabel,relatime)<br>
/dev/block/dm-15 on /data/local/tmp/rw_mounted_partitions/product_b type ext4 (rw,seclabel,relatime)<br>
/dev/block/dm-16 on /data/local/tmp/rw_mounted_partitions/system_b type ext4 (rw,seclabel,relatime)<br>
/dev/block/dm-19 on /data/local/tmp/rw_mounted_partitions/system_ext_b type ext4 (rw,seclabel,relatime)<br>
/dev/block/dm-20 on /data/local/tmp/rw_mounted_partitions/vendor_b type ext4 (rw,seclabel,relatime)<br>
/dev/block/dm-25 on /data/local/tmp/rw_mounted_partitions/vendor_dlkm_b type ext4 (rw,seclabel,relatime)<br>
ASUS_I006D:/ # <br>
	<br>
</samp>
</details>


Building
========

The source code can be compiled in Android using the **clang19 toolchain** for Android. The **clang19 toolchain** for Android is available here:

[https://github.com/bnsmb/clang19_toolchain_for_android](https://github.com/bnsmb/clang19_toolchain_for_android)

----

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

The created executable is dynamically linked but only for the standard Android libraries:
```
ASUS_I006D:/system/bin # ldd /system/bin/mount_dynamic_partitions                                                                                                                                                                                                
	linux-vdso.so.1 => [vdso] (0x7e07efe000)
	libc.so => /apex/com.android.runtime/lib64/bionic/libc.so (0x7e02e98000)
	libm.so => /apex/com.android.runtime/lib64/bionic/libm.so (0x7e06ac5000)
	libdl.so => /apex/com.android.runtime/lib64/bionic/libdl.so (0x7e06a9f000)
ASUS_I006D:/system/bin #
```

Therefore, it should run on any of the current Android ROMs and recoveries.

Usage
=====

Either clone the repository and create your own binary, or download the binary for **arm64** CPUs from the repository:
```
wget https://github.com/bnsmb/parse-android-dynparts-for-Android/raw/refs/heads/droidian/mount_dynamic_partitions
```

The documentation for mount_dynamic_partitions is available here:

[http://bnsmb.de/My_HowTos_for_Android_open_details.html#How_to_mount_the_dynamic_partitions_in_Android_in_readwrite_mode](http://bnsmb.de/My_HowTos_for_Android_open_details.html#How_to_mount_the_dynamic_partitions_in_Android_in_readwrite_mode)

(see also the readme in the GitHub repository with the original source code used for this program : [https://github.com/droidian/parse-android-dynparts](https://github.com/droidian/parse-android-dynparts) )

