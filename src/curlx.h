// Evil Tuner XXL -- MPC OS's own libcurl (/usr/lib/libcurl.so.4, 8.x, with its OpenSSL 3 and the system CA bundle), loaded
// at run time: nothing about TLS is bundled. The option numbers and function types come from libcurl's own header
// (third_party/curl, 8.5.0); their values are part of libcurl's stable ABI. docs/NOTES.md of mpc-vst-plugins records
// that MPC OS ships libcurl.so.4, libssl.so.3 and libcrypto.so.3 (checked on a Force).
#pragma once
#define CURL_DISABLE_TYPECHECK 1
#define CURL_NO_OLDIES 1
#include <curl/curl.h>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace et {

struct CurlApi {
  CURL* (*easy_init)(void) = nullptr;
  CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
  CURLcode (*easy_perform)(CURL*) = nullptr;
  void (*easy_cleanup)(CURL*) = nullptr;
  CURLcode (*easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
  const char* (*easy_strerror)(CURLcode) = nullptr;
  struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
  void (*slist_free_all)(struct curl_slist*) = nullptr;
  CURLcode (*global_init)(long) = nullptr;
  bool ok = false;
};

inline CurlApi& curl() {
  static CurlApi api;
  static std::once_flag once;
  std::call_once(once, [] {
    void* h = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    api.easy_init = (CURL * (*)(void)) dlsym(h, "curl_easy_init");
    api.easy_setopt = (CURLcode(*)(CURL*, CURLoption, ...))dlsym(h, "curl_easy_setopt");
    api.easy_perform = (CURLcode(*)(CURL*))dlsym(h, "curl_easy_perform");
    api.easy_cleanup = (void (*)(CURL*))dlsym(h, "curl_easy_cleanup");
    api.easy_getinfo = (CURLcode(*)(CURL*, CURLINFO, ...))dlsym(h, "curl_easy_getinfo");
    api.easy_strerror = (const char* (*)(CURLcode))dlsym(h, "curl_easy_strerror");
    api.slist_append = (struct curl_slist * (*)(struct curl_slist*, const char*)) dlsym(h, "curl_slist_append");
    api.slist_free_all = (void (*)(struct curl_slist*))dlsym(h, "curl_slist_free_all");
    api.global_init = (CURLcode(*)(long))dlsym(h, "curl_global_init");
    api.ok = api.easy_init && api.easy_setopt && api.easy_perform && api.easy_cleanup && api.easy_getinfo &&
             api.easy_strerror && api.slist_append && api.slist_free_all;
    if (api.ok && api.global_init) api.global_init(CURL_GLOBAL_DEFAULT);
  });
  return api;
}

// The common options: a user agent naming the app (Radio Browser asks for one), redirects, no signals (threads),
// the system CA bundle when it is where Debian-style systems keep it.
inline void curl_common(CURL* c, long connect_timeout_s) {
  CurlApi& a = curl();
  a.easy_setopt(c, CURLOPT_USERAGENT, "EvilTunerXXL/" ET_VERSION " (MPC OS plugin)");
  a.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  a.easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
  a.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  a.easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connect_timeout_s);
  struct stat st;
  if (stat("/etc/ssl/certs/ca-certificates.crt", &st) == 0)
    a.easy_setopt(c, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
}

}  // namespace et
