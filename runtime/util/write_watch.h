#pragma once

// DEBUG_WATCH_WRITE=address[,length[,frame]] (debugging; hex guest address, length in bytes, default 4,
// within one 4 KB page) logs the guest call chains that write there (the first 64 distinct ones, from the host
// stack: every guest call is a host call) and the word at the address after each write. From that frame on
// (default 0; `boot`: once the image is loaded, before any guest code runs, for the image's variables) the host
// page is write-protected; each write to it is single-stepped with the page writable, so the game runs on, slowly
// if it writes that page often. File reads into the page are let through and logged.
#include <cstdint>

namespace debug
{
    void InstallWriteWatch();
    // Called once per presented frame: protects the page from the given frame on.
    void WriteWatchOnFrame(uint64_t frame);
    // Called once the game's image is loaded, before guest code runs: protects the page now for `boot`.
    void WriteWatchOnImageLoaded();
    // The host (a file read) writes guest memory: the watched page is writable meanwhile (`done` false, then true).
    void WriteWatchHostWrite(uint32_t address, uint32_t length, bool done);
}
