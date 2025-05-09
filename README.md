# curl Multi-Threading Crash Reproducer

A minimal test program that try to reproduce [curl/curl#17256](https://github.com/curl/curl/issues/17256) but it' not 100% reliable other crashes may occur- a bug that occurs when using libcurl with the threaded resolver.

## How the Reproducer Works

This program:

1. Launches multiple worker threads, each with its own `CURLM` handle
2. Continuously adds new transfers while randomly canceling some in progress
3. Deliberately stresses the DNS resolver by:
   - Disabling DNS caching (`CURLOPT_DNS_CACHE_TIMEOUT=0`) 
   - Forbidding connection reuse (`CURLOPT_FORBID_REUSE=1`)
   - Enabling quick exit (`CURLOPT_QUICK_EXIT=1`)
4. Uses a custom `getaddrinfo` interposer via fishhook that:
   - Randomly fails with `EAI_AGAIN` (30% of calls)
   - Introduces random delays (40% of calls)

## Building on macOS

### Prerequisites

- CMake 3.25 or newer
- Clang with C++23 support
- macOS 11+ (tested on macOS 13)

### Build Steps

1. Clone this repository:
   ```bash
   git clone git@github.com:Wizermil/curl-bug-17256.git
   cd curl-bug-17256
   ```

2. Create a build directory:
   ```bash
   mkdir build && cd build
   ```

3. Configure the project:
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_INTERNAL_CURL=ON
   ```

4. Build:
   ```bash
   cmake --build . -j
   ```

The build automatically downloads and builds curl 8.13.0 from source, configured with the threaded resolver.

## Running and Debugging

### Basic Execution

Run the crasher directly:
```bash
./crasher
```

### With DNS Interposition

To simulate DNS failures and delays:
```bash
DYLD_INSERT_LIBRARIES=./libresolve_interpose.dylib ./crasher
```

### Debugging with LLDB

Start a debugging session:
```bash
lldb ./crasher
```

Useful LLDB commands:

```
# Set breakpoints for the crash
(lldb) settings set target.env-vars DYLD_INSERT_LIBRARIES=./libresolver_interpose.dylib

# Run the program
(lldb) run

# After a crash, view the backtrace
(lldb) bt
```

## Implementation Details

- `main.cpp`: Multi-threaded stress test program
- `fish_interposer.mm`: Objective-C++ implementation that uses fishhook to intercept DNS resolution calls
- `CMakeLists.txt`: Configures the build with curl from source
