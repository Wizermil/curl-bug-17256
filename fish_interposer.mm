// fish_interposer.mm - interpose getaddrinfo using fishhook on macOS
// Build as part of resolver_interpose dylib.

#include <netdb.h>
#include <atomic>
#include <random>
#include <chrono>
#include <iostream>
#include <thread>

extern "C" {
#include "fishhook.h"
}

using getaddrinfo_fn = int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **);

static std::atomic<int> calls{0};
static getaddrinfo_fn real_gai = nullptr;

static int hook_getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **res) {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static thread_local std::uniform_int_distribution<int> dice(0, 9);

  int pick = dice(rng);
  int call_no = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  std::cerr << "[fishhook] call " << call_no << " host " << (node ? node : "(null)")
            << " pick=" << pick << std::endl;

  if (pick <= 2) { // 30% probability of transient failure
    std::cerr << "[fishhook] returning EAI_AGAIN" << std::endl;
    return EAI_AGAIN;
  }
  if (pick <= 6) { // 40% probability of significant delay
    int ms = 300 + dice(rng) * 60; // 300-900 ms
    std::cerr << "[fishhook] delay " << ms << " ms for host "
              << (node ? node : "(null)") << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  } else {
    std::cerr << "[fishhook] fast resolve for host " << (node ? node : "(null)") << std::endl;
  }
  // pass-through to original
  return real_gai(node, service, hints, res);
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

__attribute__((destructor))
static void summary() {
  std::cerr << "[fishhook] total calls: " << calls.load() << std::endl;
}
