#ifndef APROTO_CONFIG_H
#define APROTO_CONFIG_H

#include <avariant.h>

// Runner startup config: one varsolid blob passed as --config <hex>.
// Keys, ranges and defaults live here — the GUI, the runner and the tests
// all read this header, so a change without its users is a compile error.
namespace proto {
    namespace config {
        constexpr const char* kTransport = "transport";
        constexpr const char* kEndpoint = "endpoint";
        constexpr const char* kToken = "token";
        constexpr const char* kTrace = "trace";
        constexpr const char* kFrameMs = "frameMs";
        constexpr const char* kFrameBytes = "frameBytes";
        constexpr const char* kTrimBytes = "trimBytes";
        constexpr const char* kLogPath = "logPath";

        constexpr alx::uint_64 kFrameMsMin = 16;
        constexpr alx::uint_64 kFrameMsMax = 500;
        constexpr alx::uint_64 kFrameBytesMin = 4u * 1024u;
        constexpr alx::uint_64 kFrameBytesMax = 8u * 1024u * 1024u;
        constexpr alx::uint_64 kTrimBytesMin = 1024u;
        constexpr alx::uint_64 kTrimBytesMax = 1024u * 1024u;

        constexpr alx::uint_64 kFrameMsDefault = 16;
        constexpr alx::uint_64 kFrameBytesDefault = 64u * 1024u;
        constexpr alx::uint_64 kTrimBytesDefault = 64u * 1024u;
    } // namespace config
} // namespace proto

#endif // APROTO_CONFIG_H
