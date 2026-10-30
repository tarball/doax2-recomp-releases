#pragma once

// What two machines must have alike to play a netplay session together (netplay/session.h, whose handshake compares
// them): the same build of the game's executable, the same disc image, and the same switches (netplay/frame.h); and for
// the log, what each machine is.
#include <cstdint>
#include <optional>
#include <string>

namespace netplay::identity
{
    // A hash of the game's executable and its code module (the files, as built; ANALYSIS.md, Phase 5, The game's code
    // module), but for what differs between two links of the same code: the PE header's time stamp and checksum, and the
    // debug directory, which names the PDB. 0 if the executable can't be read. Computed once, from the files rather than
    // the images in memory, which the loader relocates wherever they go.
    uint64_t BuildId();
    // The build's configuration: RelWithDebInfo, Release or Debug.
    const char* BuildConfiguration();
    // Starts finding the disc image's hash (config::DiscImage): from the data directory's disc_hash.txt if it has the
    // image's, by its path, size and modification time, or else by hashing the whole image on a thread of its own (a few
    // seconds for its 7.8 GB on an SSD) and keeping the result there.
    void StartDiscHash();
    // The disc image's hash, once known (0 if the image can't be read).
    std::optional<uint64_t> DiscHash();
    // The CPU (its brand string, and whether it has FMA3) and the Windows build, for the log.
    std::string Machine();
}
