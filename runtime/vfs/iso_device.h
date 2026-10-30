#pragma once

#include "vfs/vfs.h"

namespace vfs
{
    // Read-only XDVDFS (Xbox game disc) file system read directly from a disc image.
    std::unique_ptr<Device> OpenIsoDevice(const std::string& isoPath);

    // A writable device backed by a host directory (created if missing).
    std::unique_ptr<Device> OpenHostDevice(const std::string& name, const std::string& hostDirectory);

    // A hard disk partition: its file system is a host directory (none if `hostDirectory` is empty: Partition0, the whole
    // disk, has none), and opening the partition itself gives its raw sectors, kept in the sparse host file `rawStore`.
    // The partition's place on the disk (`offset`, `length`) is what the disk queries report. Formatting it as FATX
    // writes a superblock there, whose cluster size the file system then reports.
    struct PartitionLayout
    {
        uint64_t offset = 0;
        uint64_t length = 0;
    };
    std::unique_ptr<Device> OpenHardDiskPartition(const std::string& name, const std::string& hostDirectory, const std::string& rawStore,
        PartitionLayout layout);
}
