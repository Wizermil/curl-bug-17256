// fish_interposer.mm - interpose getaddrinfo using fishhook on macOS
// Build as part of resolver_interpose dylib.

#include <netdb.h>
#include <atomic>
#include <random>
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <utility>

// Define EAI_CANCELED if not available in system headers
// This constant is not available in all platforms/versions
#ifndef EAI_CANCELED
#define EAI_CANCELED -4000  // Custom value that doesn't conflict with standard ones
#endif

extern "C" {
#include "fishhook.h"
}

using getaddrinfo_fn = int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **);

static std::atomic<int> calls{0};
static getaddrinfo_fn real_gai = nullptr;
static std::mutex gai_mutex; // Protect parallel calls to getaddrinfo

// Track active thread operations to prevent use-after-free
static std::mutex thread_map_mutex;
static std::unordered_map<std::thread::id, bool> active_threads;

// All possible getaddrinfo error codes with their descriptions and probability weights
// Higher weight means more likely to be selected
struct ErrorCode {
    int code;               // The error code
    const char* name;       // Name of the error code
    const char* description;// Description of the error
    int weight;             // Probability weight (higher = more likely)
};

static const std::vector<ErrorCode> ERROR_CODES = {
    {EAI_AGAIN,      "EAI_AGAIN",      "Temporary failure in name resolution", 30},
    {EAI_BADFLAGS,   "EAI_BADFLAGS",   "Invalid value for ai_flags", 5},
    {EAI_FAIL,       "EAI_FAIL",       "Non-recoverable failure in name resolution", 10},
    {EAI_FAMILY,     "EAI_FAMILY",     "ai_family not supported", 5},
    {EAI_MEMORY,     "EAI_MEMORY",     "Memory allocation failure", 5},
    {EAI_NONAME,     "EAI_NONAME",     "Name or service not known", 20},
    {EAI_SERVICE,    "EAI_SERVICE",    "Service not supported for socket type", 5},
    {EAI_SOCKTYPE,   "EAI_SOCKTYPE",   "ai_socktype not supported", 5},
    {EAI_SYSTEM,     "EAI_SYSTEM",     "System error returned in errno", 10},
#ifdef EAI_OVERFLOW
    {EAI_OVERFLOW,   "EAI_OVERFLOW",   "Argument buffer overflow", 5},
#endif
#ifdef EAI_NODATA
    {EAI_NODATA,     "EAI_NODATA",     "No address associated with hostname", 5},
#endif
#ifdef EAI_ADDRFAMILY
    {EAI_ADDRFAMILY, "EAI_ADDRFAMILY", "Address family for hostname not supported", 5},
#endif
    // EAI_CANCELED is defined above if not provided by the system
    {EAI_CANCELED,   "EAI_CANCELED",   "Request canceled", 10},
};

// Total weight for probability calculation
static const int TOTAL_ERROR_WEIGHT = []() {
    int total = 0;
    for (const auto& err : ERROR_CODES) {
        total += err.weight;
    }
    return total;
}();

// Refined helper function with exponential backoff for mutex lock retries
static void with_mutex_retry(std::mutex& mut, const char* context, auto&& func) {
    int attempt = 0;
    const int max_attempts = 5;
    while (attempt < max_attempts) {
        try {
            std::lock_guard<std::mutex> lock(mut);
            func(); // Execute the protected code
            return;
        } catch (const std::system_error& e) {
            if (e.code() == std::errc::invalid_argument || e.code() == std::errc::operation_not_permitted) {
                int delay_ms = 10 * (1 << attempt); // Exponential backoff: 10, 20, 40, 80, 160 ms
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                attempt++;
                if (attempt == max_attempts) {
                    std::cerr << "[error] Failed to lock " << context << " after retries: " << e.what() << std::endl;
                    return; // Handle failure
                }
            } else {
                throw; // Rethrow unexpected errors
            }
        } catch (...) {
            throw; // Propagate other exceptions
        }
    }
}

static int hook_getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **res) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static thread_local std::uniform_int_distribution<int> dice(0, 99); // 0-99 for percentages
  static thread_local std::uniform_int_distribution<int> error_dice(1, TOTAL_ERROR_WEIGHT);
  auto thread_id = std::this_thread::get_id();
  
  // Register this thread as active
  {
    with_mutex_retry(thread_map_mutex, "thread map registration", [&]() { active_threads[thread_id] = true; });
  }
  
  int pick = dice(rng);
  int call_no = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  std::cerr << "\t[getaddrinfo] call " << call_no << " host " << (node ? node : "(null)")
            << " pick=" << pick << std::endl;

  int result;
  try {
    if (pick < 40) { // 40% probability of failure with various error codes
      // Select an error code based on weighted probability
      int error_pick = error_dice(rng);
      int cumulative_weight = 0;
      
      const ErrorCode* selected_error = nullptr;
      
      for (const auto& err : ERROR_CODES) {
        cumulative_weight += err.weight;
        if (error_pick <= cumulative_weight) {
          selected_error = &err;
          break;
        }
      }
      
      if (!selected_error) {
        // Fallback in case of calculation error
        selected_error = &ERROR_CODES[0]; // Default to EAI_AGAIN
      }
      
      std::cerr << "\t[getaddrinfo] returning " << selected_error->name 
                << ": " << selected_error->description << std::endl;
      result = selected_error->code;
    } else {
      if (pick < 80) { // 40% probability of significant delay
        int ms = 300 + dice(rng) % 600; // 300-900 ms
        std::cerr << "\t[getaddrinfo] delay " << ms << " ms for host "
                  << (node ? node : "(null)") << std::endl;
        
        // Use a shorter sleep duration and check if we're still active periodically
        // to allow for clean cancellation
        const int sleep_chunk = 50; // 50ms chunks
        for (int slept = 0; slept < ms; slept += sleep_chunk) {
          std::this_thread::sleep_for(std::chrono::milliseconds(sleep_chunk));
          
          // Check if this thread is still expected to be running
          bool still_active = false;
          {
            with_mutex_retry(thread_map_mutex, "thread map activity check", [&]() {
              auto it = active_threads.find(thread_id);
              if (it != active_threads.end()) {
                still_active = it->second;
              }
            });
          }
          
          if (!still_active) {
            std::cerr << "\t[getaddrinfo] thread operation canceled during delay" << std::endl;
            result = EAI_CANCELED;
            goto cleanup;
          }
        }
      } else {
        std::cerr << "\t[getaddrinfo] fast resolve for host " << (node ? node : "(null)") << std::endl;
      }
      
      // Protect the real getaddrinfo call with a mutex to avoid concurrent calls
      // which might lead to heap-use-after-free in curl's threaded resolver
      try {
        with_mutex_retry(gai_mutex, "gai mutex", [&]() {
          result = real_gai(node, service, hints, res);
        });
      } catch (const std::exception& e) {
        std::cerr << "\t[getaddrinfo] exception in getaddrinfo interpose: " << e.what() << std::endl;
        result = EAI_SYSTEM;
      } catch (...) {
        std::cerr << "\t[getaddrinfo] unknown exception in getaddrinfo interpose" << std::endl;
        result = EAI_SYSTEM;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "\t[getaddrinfo] exception in getaddrinfo interpose: " << e.what() << std::endl;
    result = EAI_SYSTEM;
  } catch (...) {
    std::cerr << "\t[getaddrinfo] unknown exception in getaddrinfo interpose" << std::endl;
    result = EAI_SYSTEM;
  }
  
cleanup:
  // Unregister this thread when done
  {
    with_mutex_retry(thread_map_mutex, "thread map unregister", [&]() { active_threads.erase(thread_id); });
  }
  
  return result;
}

__attribute__((constructor))
static void install_hook() {
  std::cerr << "[fishhook] installing hook" << std::endl;
  struct rebinding rb{ "getaddrinfo", (void *)hook_getaddrinfo, (void **)&real_gai };
  int ret = rebind_symbols(&rb, 1);
  if (ret == 0)
    std::cerr << "[fishhook] hook installed" << std::endl;
  else
    std::cerr << "[fishhook] rebind_symbols failed: " << ret << std::endl;
}

// Clean termination of active threads
static void cleanup_active_threads() {
  try {
    with_mutex_retry(thread_map_mutex, "cleanup mutex", [&]() {
      std::cerr << "[fishhook] cleaning up " << active_threads.size() << " active threads" << std::endl;
      active_threads.clear();
    });
  } catch (const std::system_error& e) {
    std::cerr << "[fishhook] error during cleanup: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[fishhook] unknown error during cleanup" << std::endl;
  }
}

__attribute__((destructor))
static void summary() {
  cleanup_active_threads();
  std::cerr << "[fishhook] total calls: " << calls.load() << std::endl;
}
