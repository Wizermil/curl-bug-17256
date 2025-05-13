// hook_getaddrinfo.cpp - interpose getaddrinfo using fishhook on macOS
// Build as part of resolver_interpose dylib.

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Define EAI_CANCELED if not available in system headers
// This constant is not available in all platforms/versions
#ifndef EAI_CANCELED
#define EAI_CANCELED -4000 // Custom value that doesn't conflict with standard ones
#endif

extern "C"
{
#include "fishhook.h"
}

using getaddrinfo_fn = int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
static getaddrinfo_fn real_gai = nullptr;
static std::mutex gai_mutex;

#ifdef MYAPP_LOGGING_ENABLED
// Thread-safe logging for interposer (outputs to stderr)
static std::mutex interposer_log_mutex;

template <typename... Args>
void log_interposer(Args &&...args)
{
  std::ostringstream stream;
  (stream << ... << std::forward<Args>(args));

  std::lock_guard<std::mutex> lock(interposer_log_mutex);
  std::cerr << stream.str() << std::endl;
}
#else  // MYAPP_LOGGING_ENABLED
// Empty inline function when logging is disabled
template <typename... Args>
inline void log_interposer(Args &&...) {}
#endif // MYAPP_LOGGING_ENABLED

static consteval int ErrorCodeMax()
{
  int ret = 10;
#ifdef EAI_OVERFLOW
  ret += 1;
#endif
#ifdef EAI_NODATA
  ret += 1;
#endif
#ifdef EAI_ADDRFAMILY
  ret += 1;
#endif
  return ret;
}

// All possible getaddrinfo error codes with their descriptions and probability weights
// Higher weight means more likely to be selected
struct ErrorCode
{
  int code;                // The error code
  const char *name;        // Name of the error code
  const char *description; // Description of the error
  int weight;              // Probability weight (higher = more likely)
};

static constexpr std::array<ErrorCode, ErrorCodeMax()> ERROR_CODES = {
    ErrorCode{EAI_AGAIN, "EAI_AGAIN", "Temporary failure in name resolution", 30},
    ErrorCode{EAI_BADFLAGS, "EAI_BADFLAGS", "Invalid value for ai_flags", 5},
    ErrorCode{EAI_FAIL, "EAI_FAIL", "Non-recoverable failure in name resolution", 10},
    ErrorCode{EAI_FAMILY, "EAI_FAMILY", "ai_family not supported", 5},
    ErrorCode{EAI_MEMORY, "EAI_MEMORY", "Memory allocation failure", 5},
    ErrorCode{EAI_NONAME, "EAI_NONAME", "Name or service not known", 20},
    ErrorCode{EAI_SERVICE, "EAI_SERVICE", "Service not supported for socket type", 5},
    ErrorCode{EAI_SOCKTYPE, "EAI_SOCKTYPE", "ai_socktype not supported", 5},
    ErrorCode{EAI_SYSTEM, "EAI_SYSTEM", "System error returned in errno", 10},
#ifdef EAI_OVERFLOW
    ErrorCode{EAI_OVERFLOW, "EAI_OVERFLOW", "Argument buffer overflow", 5},
#endif
#ifdef EAI_NODATA
    ErrorCode{EAI_NODATA, "EAI_NODATA", "No address associated with hostname", 5},
#endif
#ifdef EAI_ADDRFAMILY
    ErrorCode{EAI_ADDRFAMILY, "EAI_ADDRFAMILY", "Address family for hostname not supported", 5},
#endif
    // EAI_CANCELED is defined above if not provided by the system
    ErrorCode{EAI_CANCELED, "EAI_CANCELED", "Request canceled", 10},
};

// Total weight for probability calculation
static consteval int TOTAL_ERROR_WEIGHT()
{
  int total = 0;
  for (const auto &err : ERROR_CODES)
  {
    total += err.weight;
  }
  return total;
}

static int hook_getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **res)
{
  const std::lock_guard<std::mutex> lock(gai_mutex);
  static thread_local std::mt19937 rng{std::random_device{}()};
  static thread_local std::uniform_int_distribution<int> dice(0, 99); // 0-99 for percentages
  static thread_local std::uniform_int_distribution<int> error_dice(1, TOTAL_ERROR_WEIGHT());
  int pick = dice(rng);
  log_interposer("\t[getaddrinfo] host ", (node ? node : "(null)"), " pick=", pick);

  int result;
  if (pick < 40)
  { // 40% probability of failure with various error codes
    // Select an error code based on weighted probability
    int error_pick = error_dice(rng);
    int cumulative_weight = 0;

    const ErrorCode *selected_error = nullptr;

    for (const auto &err : ERROR_CODES)
    {
      cumulative_weight += err.weight;
      if (error_pick <= cumulative_weight)
      {
        selected_error = &err;
        break;
      }
    }

    if (!selected_error)
    {
      // Fallback in case of calculation error
      selected_error = &ERROR_CODES[0]; // Default to EAI_AGAIN
    }

    log_interposer("\t[getaddrinfo] returning ", selected_error->name, ": ", selected_error->description);
    result = selected_error->code;
  }
  else
  {
    if (pick < 80)
    {
      int ms = 3 + dice(rng) % 300; // 3-300 ms
      log_interposer("\t[getaddrinfo] delay ", ms, " ms for host ", (node ? node : "(null)"));
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    else
    {
      log_interposer("\t[getaddrinfo] fast resolve for host ", (node ? node : "(null)"));
    }

    // Protect the real getaddrinfo call with a mutex to avoid concurrent calls
    // which might lead to heap-use-after-free in curl's threaded resolver
    result = real_gai(node, service, hints, res);
  }
  return result;
}

__attribute__((constructor)) static void init()
{
  const std::lock_guard<std::mutex> lock(gai_mutex);
  log_interposer("[fishhook] Initializing getaddrinfo interposer (constructor)");
  struct rebinding rebindings[1];
  rebindings[0] = {"getaddrinfo", (void *)hook_getaddrinfo, (void **)&real_gai};
  rebind_symbols(rebindings, 1);
}
