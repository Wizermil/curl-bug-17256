// Stress-test program: spawns multiple threads, each with its own CURLM handle.
// Each thread continuously queues new transfers, randomly cancels some in-flight
// handles, and runs its own poll/perform loop.

#include <algorithm>
#include <array>
#include <chrono>
#include <curl/curl.h>
#include <iostream>
#include <mutex>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

// Thread-safe logging
#ifdef MYAPP_LOGGING_ENABLED
std::mutex cout_mutex;

template <typename... Args>
void log(Args &&...args)
{
  std::ostringstream stream;
  (stream << ... << std::forward<Args>(args));

  std::lock_guard<std::mutex> lock(cout_mutex);
  std::cout << stream.str() << std::endl;
}
#else  // MYAPP_LOGGING_ENABLED
// Empty inline function when logging is disabled
template <typename... Args>
inline void log(Args &&...) {}
#endif // MYAPP_LOGGING_ENABLED

// Discard body callback — we do not need the payload
static size_t sink(char * /*ptr*/, size_t size, size_t nmemb, void *)
{
  return size * nmemb;
}

// Progress callback that randomly aborts after >1MB downloaded
static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
  // clientp now points to the C-string (const char*) of the URL
  const char *url_cstr = static_cast<const char *>(clientp);

  // Log progress for debugging
  if (dlnow > 0 && dltotal > 0)
  {
    log("[progress] ", url_cstr, " downloaded ", dlnow / 1024, "/", dltotal / 1024, " KiB");
  }

  if (dlnow < 100 * 1024)
    return 0;

  static std::mt19937 rng{std::random_device{}()};
  static std::uniform_int_distribution<int> dice(0, 4); // 20% chance
  bool abort = dice(rng) == 0;
  if (abort)
  {
    log("[cancel] Aborting download of ", url_cstr, " after ", dlnow / 1024, " KiB");
    return 1;
  }
  return 0;
}

static CURL *add_easy(CURLM *multi, std::string_view url_sv, bool enable_cancel = false)
{
  log("[queue] ", url_sv);
  CURL *easy = curl_easy_init();

  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2);
  curl_easy_setopt(easy, CURLOPT_URL, url_sv.data());
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sink);

  // Critical options to tickle the crash
  curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, 0L); // disable DNS cache
  curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);      // fresh conn each time
  curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);        // suspected factor

  if (enable_cancel)
  {
    // Always enable progress monitoring
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, progress_cb);
    // Pass the persistent pointer to the URL string (now url_sv.data(), which is const char*)
    // clientp for progress_cb will be const char*
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, (void *)url_sv.data()); // Cast to void*
  }

  curl_multi_add_handle(multi, easy);
  return easy;
}

// Worker owning its own CURLM handle
static void worker_thread(int id, std::span<const std::string_view> urls, std::chrono::seconds duration)
{
  CURLM *multi = curl_multi_init();
  std::vector<CURL *> handles;

  std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> pick10(0, 9);
  std::uniform_int_distribution<size_t> url_pick(0, urls.size() - 1);

  auto deadline = std::chrono::steady_clock::now() + duration;
  int running = 0;

  while (std::chrono::steady_clock::now() < deadline)
  {
    // keep up to 5 concurrent transfers
    while (handles.size() < 5)
    {
      const std::string_view &u = urls[url_pick(rng)];
      handles.push_back(add_easy(multi, u, true));
    }

    // Perform transfers
    curl_multi_perform(multi, &running);
    int numfds = 0;
    curl_multi_poll(multi, nullptr, 0, 200, &numfds);

    // Reap finished
    int msgs_left = 0;
    while (CURLMsg *msg = curl_multi_info_read(multi, &msgs_left))
    {
      if (msg->msg == CURLMSG_DONE)
      {
        CURL *e = msg->easy_handle;
        curl_multi_remove_handle(multi, e);
        curl_easy_cleanup(e);
        handles.erase(std::remove(handles.begin(), handles.end(), e), handles.end());
      }
    }

    // Random cancellation
    if (!handles.empty() && pick10(rng) == 0)
    {
      size_t idx = rng() % handles.size();
      CURL *e = handles[idx];
      log("[cancel] thread ", id, " removing handle");
      curl_multi_remove_handle(multi, e);
      curl_easy_cleanup(e);
      handles.erase(handles.begin() + idx);
    }
  }

  // Cleanup remaining
  for (CURL *e : handles)
  {
    curl_multi_remove_handle(multi, e);
    curl_easy_cleanup(e);
  }
  curl_multi_cleanup(multi);
  log("[thread] ", id, " finished");
}

// All URLs for stress testing organized by category
static constexpr std::array<std::string_view, 68> all_test_urls = {
    // Small files (< 1MB)
    "https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc",
    "https://raw.githubusercontent.com/curl/curl/master/README",
    "https://speed.hetzner.de/100KB.bin",
    "https://speed.hetzner.de/1MB.bin",

    // Medium files (1-10MB)
    "https://speed.hetzner.de/10MB.bin",
    "https://www.learningcontainer.com/wp-content/uploads/2020/05/sample-5mb.pdf",
    "https://proof.ovh.net/files/5Mb.dat",

    // Large files (> 10MB) - use with caution as they might slow down tests
    "https://speed.hetzner.de/100MB.bin",
    "https://proof.ovh.net/files/100Mb.dat",

    // Specific file types
    "https://www.w3.org/WAI/ER/tests/xhtml/testfiles/resources/pdf/dummy.pdf",
    "https://file-examples.com/storage/fe2a41b7b56438da93df486/2017/04/file_example_MP4_480_1_5MG.mp4",
    "https://file-examples.com/storage/fe2a41b7b56438da93df486/2017/11/file_example_MP3_700KB.mp3",
    "https://file-examples.com/storage/fe2a41b7b56438da93df486/2017/10/file_example_PNG_500kB.png",

    // HTTPS with redirects
    "https://bit.ly/3y0UWGJ",
    "https://httpbin.org/redirect/3",

    // Server with special behavior
    "https://httpbin.org/delay/2",
    "https://httpbin.org/status/429",
    "https://httpbin.org/status/500",
    "https://httpbin.org/status/404",

    // IPv6 enabled servers
    "https://ipv6.google.com/",
    "https://ipv6.cloudflare-dns.com/",

    // Popular CDNs
    "https://ajax.googleapis.com/ajax/libs/jquery/3.6.0/jquery.min.js",
    "https://cdnjs.cloudflare.com/ajax/libs/jquery/3.6.0/jquery.min.js",
    "https://cdn.jsdelivr.net/npm/bootstrap@5.1.3/dist/css/bootstrap.min.css",
    "https://unpkg.com/react@17/umd/react.production.min.js",

    // Cloud storage providers
    "https://storage.googleapis.com/pub-tools-public-publication-data/pdf/1e476f4d97eecc7f3673d74cbce0387a15a8ab53.pdf",
    "https://download.microsoft.com/download/9/3/F/93FCF1E7-E6A4-478B-96E7-D4B285925B00/GUID-4.pdf",
    "https://aws.amazon.com/lambda/resources/",
    "https://dl.fbaipublications.com/fasttext/vectors-crawl/cc.en.300.bin.gz",

    // Government sites
    "https://www.nasa.gov/wp-content/themes/nasa/assets/images/nasa-logo.svg",
    "https://www.whitehouse.gov/",
    "https://www.parliament.uk/",
    "https://europa.eu/european-union/index_en",

    // University sites
    "https://www.ox.ac.uk/",
    "https://www.harvard.edu/",
    "https://www.stanford.edu/",
    "https://www.mit.edu/",

    // Different file types
    "https://www.w3.org/TR/PNG/iso_8859-1.txt",
    "https://www.w3.org/People/mimasa/test/imgformat/img/w3c_home.jpg",
    "https://filesamples.com/samples/document/csv/sample1.csv",
    "https://filesamples.com/samples/code/json/sample1.json",

    // Redirects and special cases
    "https://httpstat.us/200",
    "https://httpstat.us/301",
    "https://httpstat.us/400",
    "https://httpstat.us/503",

    // International domains
    "https://www.bbc.co.uk/",
    "https://www.tagesschau.de/",
    "https://www.nhk.or.jp/",
    "https://www.rtve.es/",

    // Media streaming and large files
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4", // Large file
    "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/TearsOfSteel.mp4", // Large file

    // API endpoints with different response types
    "https://api.chucknorris.io/jokes/random",
    "https://cat-fact.herokuapp.com/facts/random",
    "https://api.publicapis.org/entries",
    "https://jsonplaceholder.typicode.com/posts",

    // Different network challenges
    "https://deelay.me/1000/https://example.com", // 1 second delay
    "https://deelay.me/3000/https://example.com", // 3 second delay

    // Health check endpoints
    "https://status.github.com/api/status.json",
    "https://www.githubstatus.com/",
    "https://status.cloud.google.com/",
    "https://status.aws.amazon.com/",

    // Additional variety for volume testing
    "https://archive.org/download/BigBuckBunny_124/Content/big_buck_bunny_720p_surround.mp4",
    "https://cdn.shopify.com/s/files/1/0155/7645/products/cover_efa16558-4f83-4c39-941a-193fa9bc6854_large.jpg",
    "https://soundhelix.com/examples/mp3/SoundHelix-Song-1.mp3",
    "https://fonts.googleapis.com/css?family=Roboto:300,400,500,700",
    "https://www.php.net/distributions/php-8.0.0.tar.gz",
    "https://www.python.org/ftp/python/3.9.7/Python-3.9.7.tar.xz",
    "https://nodejs.org/dist/v14.17.6/node-v14.17.6.tar.gz"};

int main()
{
  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Logging URL count for verification
  log("Using ", std::size(all_test_urls), " URLs for stress testing");

  // Use span over array directly
  const int num_threads = 8;
  static std::mt19937 rng{std::random_device{}()};
  static std::uniform_int_distribution<int> dice(1, 30);
  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i)
  {
    threads.emplace_back(worker_thread, i, std::span(all_test_urls), std::chrono::seconds(dice(rng)));
  }

  for (auto &t : threads)
    t.join();

  curl_global_cleanup();
  log("Finished stress run.");
  return 0;
}
