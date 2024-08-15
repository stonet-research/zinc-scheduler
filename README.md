# README

ZINC - A ZNS Interference-aware NVMe Command Scheduler is a configurable I/O scheduler for NVMe ZNS SSDs.
It allows prioritizing I/O commands (write, read) over I/O management operations (reset, finish).

[!WARNING]
ZINC is based on Linux 6.3.8 and relies on it codebase, it is not evaluated on other Linux versions and might require extensive modifications to run it on other kernel versions.

## How to install

1. clone this repository:

```bash
git clone https://github.com/stonet-research/zinc-scheduler.git
cd zinc-scheduler
```

2. Clones the linux block layer for [Linux 6.3](https://github.com/torvalds/linux/tree/v6.3/block) and copy the Linux directory directly into this repository

```bash
./install-linux-6.sh
```

3. Build ZINC

```bash
cp Makefile linux-6.3.8/block/
cp zinc.c linux-6.3.8/block/
cd linux-6.3.8/block/

# Make module
make
```

## How to use ZINC

1. Ensure that ZINC is build (see `How to install`).

2. Insert the ZINC module (needs to be done on each recompilation or system reboot):

```bash
cd linux-6.3.8/block/
# Install the module
sudo insmod zinc.ko
# If it fails because it is already in use, run: `sudo rmmod zinc`.
```

3. Assign to NVMe ZNS device (can be used like any Linux I/O scheduler):

```bash
echo zinc | sudo tee /sys/block/nvme*n*/queue/scheduler # nvme*n* is the device name.
```

## Configuring ZINC

### Configuration options

* reset_timer_interval: window when to retry issuing a reset in milliseconds
* write_ratio: the number of write requests before a reset can be issued (in 8 KiB units)
* pending_requests_threshold: below this number of in-flight write requests, resets are not stalled (no scheduling, also in 8 kiB units)
* max_priority: number of retries for reset (to prevent reset starvation)

## How to configure

1. First assign ZINC to an NVMe device (see `How to use ZINC`)
2. Change the configuration options (in `/sys/block/nvme*n*/queue/iosched/`), for example to set the `reset_timer_interval` do:

```bash
echo 64 | sudo tee /sys/block/nvme*n*/queue/iosched/reset_timer_interval
```
