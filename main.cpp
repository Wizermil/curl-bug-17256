// Stress-test program: spawns multiple threads, each with its own CURLM handle.
// Each thread continuously queues new transfers, randomly cancels some in-flight
// handles, and runs its own poll/perform loop.

#include <curl/curl.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// Thread-safe logging
std::mutex cout_mutex;

template <typename... Args>
void log(Args&&... args) {
    std::ostringstream stream;
    (stream << ... << std::forward<Args>(args));
    
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << stream.str() << std::endl;
}

// Discard body callback — we do not need the payload
static size_t sink(char *ptr, size_t size, size_t nmemb, void *) {
  return size * nmemb;
}

// Progress callback that randomly aborts after >1MB downloaded
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  const char *url = static_cast<const char *>(clientp);
  if (dlnow < 100 * 1024)
    return 0;
  static std::mt19937 rng{std::random_device{}()};
  static std::uniform_int_distribution<int> dice(0, 4); // 20% chance
  bool abort = dice(rng) == 0;
  if (abort) {
    log("[cancel] Aborting download of ", url, " after ", dlnow / 1024, " KiB");
    return 1;
  }
  return 0;
}

static CURL *add_easy(CURLM *multi, const std::string &url, bool enable_cancel = false) {
  log("[queue] ", url);
  CURL *easy = curl_easy_init();
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2);
  curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sink);

  // Critical options to tickle the crash
  curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, 0L); // disable DNS cache
  curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);      // fresh conn each time
  curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);         // suspected factor

  if (enable_cancel) {
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, url.c_str());
  }

  curl_multi_add_handle(multi, easy);
  return easy;
}

// Worker owning its own CURLM handle
static void worker_thread(int id, const std::vector<std::string> &urls, std::chrono::seconds duration) {
  CURLM *multi = curl_multi_init();
  std::vector<CURL *> handles;

  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> pick10(0, 9);
  std::uniform_int_distribution<size_t> url_pick(0, urls.size() - 1);

  auto deadline = std::chrono::steady_clock::now() + duration;
  int running = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    // keep up to 5 concurrent transfers
    while (handles.size() < 5) {
      const std::string &u = urls[url_pick(rng)];
      handles.push_back(add_easy(multi, u, true));
    }

    // Perform transfers
    curl_multi_perform(multi, &running);
    int numfds = 0;
    curl_multi_poll(multi, nullptr, 0, 200, &numfds);

    // Reap finished
    int msgs_left = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi, &msgs_left)) {
      if (msg->msg == CURLMSG_DONE) {
        CURL *e = msg->easy_handle;
        curl_multi_remove_handle(multi, e);
        curl_easy_cleanup(e);
        handles.erase(std::remove(handles.begin(), handles.end(), e), handles.end());
      }
    }

    // Random cancellation
    if (!handles.empty() && pick10(rng) == 0) {
      size_t idx = rng() % handles.size();
      CURL *e = handles[idx];
      log("[cancel] thread ", id, " removing handle");
      curl_multi_remove_handle(multi, e);
      curl_easy_cleanup(e);
      handles.erase(handles.begin() + idx);
    }
  }

  // Cleanup remaining
  for (CURL *e : handles) {
    curl_multi_remove_handle(multi, e);
    curl_easy_cleanup(e);
  }
  curl_multi_cleanup(multi);
  log("[thread] ", id, " finished");
}

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  std::vector<std::string> urls;
  // Generate 100 additional synthetic domains (example0.com ... example99.com)
  urls.reserve(urls.size() + 100);
  for (int i = 0; i < 100; ++i) {
    urls.emplace_back("https://example" + std::to_string(i) + ".com/");
  }

  constexpr int kThreads = 4;
  std::vector<std::thread> workers;
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back(worker_thread, i, std::cref(urls), std::chrono::seconds(30));
  }

  for (auto &t : workers) t.join();

  curl_global_cleanup();
  log("Finished stress run.");
  return 0;
}
