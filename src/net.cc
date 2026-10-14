// Evil Tuner XXL -- the station directory: Radio Browser (https://www.radio-browser.info, a free, open, community directory
// of 30,000+ stations, no account or key) and the favourites file.
//
// Radio Browser's own client guidance: find the servers through DNS (all.api.radio-browser.info resolves to every
// server; each address's reverse name is that server), send a user agent naming the app, and count a play with
// /json/url/<stationuuid>. Searches go to /json/stations/search (JSON). Everything runs on this file's worker thread;
// the audio thread polls the result with try_lock.
#include "et.h"
#include "curlx.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <map>

namespace et {

// ------------------------------------------------------------------------------------------------ a small JSON reader
// Enough for Radio Browser's answers: an array of flat objects. Values are kept as text (numbers and booleans too).
namespace json {
struct P {
  const char* s;
  const char* e;
  void ws() { while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) ++s; }
  static void utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
  }
  bool str(std::string& o) {
    if (s >= e || *s != '"') return false;
    ++s;
    while (s < e && *s != '"') {
      if (*s == '\\' && s + 1 < e) {
        ++s;
        char c = *s++;
        if (c == 'n') o += '\n';
        else if (c == 't') o += '\t';
        else if (c == 'r' || c == 'b' || c == 'f') o += ' ';
        else if (c == 'u' && s + 4 <= e) {
          unsigned cp = (unsigned)strtoul(std::string(s, 4).c_str(), nullptr, 16);
          s += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && s + 6 <= e && s[0] == '\\' && s[1] == 'u') {
            unsigned lo = (unsigned)strtoul(std::string(s + 2, 4).c_str(), nullptr, 16);
            s += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(o, cp);
        } else o += c;
      } else o += *s++;
    }
    if (s < e) ++s;
    return true;
  }
  bool scalar(std::string& o) {   // number / true / false / null, as text
    const char* b = s;
    while (s < e && *s != ',' && *s != '}' && *s != ']' && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') ++s;
    o.assign(b, s);
    if (o == "null") o.clear();
    return s > b;
  }
  bool skip() {   // any value
    ws();
    if (s >= e) return false;
    if (*s == '"') { std::string t; return str(t); }
    if (*s == '{' || *s == '[') {
      char open = *s, close = open == '{' ? '}' : ']';
      int depth = 0;
      while (s < e) {
        if (*s == '"') { std::string t; str(t); continue; }
        if (*s == open) depth++;
        else if (*s == close && --depth == 0) { ++s; return true; }
        ++s;
      }
      return false;
    }
    std::string t;
    return scalar(t);
  }
  bool object(std::map<std::string, std::string>& o) {
    ws();
    if (s >= e || *s != '{') return false;
    ++s;
    for (;;) {
      ws();
      if (s < e && *s == '}') { ++s; return true; }
      std::string k, v;
      if (!str(k)) return false;
      ws();
      if (s >= e || *s != ':') return false;
      ++s;
      ws();
      if (s < e && *s == '"') { if (!str(v)) return false; o[k] = v; }
      else if (s < e && (*s == '{' || *s == '[')) { if (!skip()) return false; }
      else { if (!scalar(v)) return false; o[k] = v; }
      ws();
      if (s < e && *s == ',') { ++s; continue; }
      if (s < e && *s == '}') { ++s; return true; }
      return false;
    }
  }
};

static bool array_of_objects(const std::string& text, std::vector<std::map<std::string, std::string>>& out) {
  P p{text.data(), text.data() + text.size()};
  p.ws();
  if (p.s >= p.e || *p.s != '[') return false;
  ++p.s;
  for (;;) {
    p.ws();
    if (p.s < p.e && *p.s == ']') return true;
    std::map<std::string, std::string> o;
    if (!p.object(o)) return false;
    out.push_back(std::move(o));
    p.ws();
    if (p.s < p.e && *p.s == ',') { ++p.s; continue; }
    return p.s < p.e && *p.s == ']';
  }
}
}  // namespace json

// ------------------------------------------------------------------------------------------------ http
static size_t collect(char* p, size_t sz, size_t n, void* ud) {
  std::string* s = static_cast<std::string*>(ud);
  if (s->size() > 4 * 1024 * 1024) return 0;
  s->append(p, sz * n);
  return sz * n;
}

static bool http_get(const std::string& url, std::string& body, long& code, std::string& err) {
  CurlApi& cu = curl();
  if (!cu.ok) { err = "NO LIBCURL"; return false; }
  CURL* c = cu.easy_init();
  if (!c) { err = "CURL INIT"; return false; }
  body.clear();
  curl_common(c, 8);
  cu.easy_setopt(c, CURLOPT_URL, url.c_str());
  cu.easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  cu.easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  cu.easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
  cu.easy_setopt(c, CURLOPT_WRITEDATA, &body);
  CURLcode rc = cu.easy_perform(c);
  code = 0;
  cu.easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  cu.easy_cleanup(c);
  if (rc != CURLE_OK) { err = cu.easy_strerror(rc); return false; }
  if (code != 200) { char b[32]; snprintf(b, sizeof b, "HTTP %ld", code); err = b; return false; }
  return true;
}

static std::string urlenc(const std::string& s) {
  std::string o;
  char b[4];
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(b, sizeof b, "%%%02X", c); o += b; }
  }
  return o;
}

// The servers: every address all.api.radio-browser.info resolves to, by its reverse name (https needs the name).
static std::vector<std::string> find_servers() {
  std::vector<std::string> out;
  struct addrinfo hints, *res = nullptr;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("all.api.radio-browser.info", "443", &hints, &res) == 0) {
    for (struct addrinfo* a = res; a; a = a->ai_next) {
      char host[256];
      if (getnameinfo(a->ai_addr, a->ai_addrlen, host, sizeof host, nullptr, 0, NI_NAMEREQD) == 0) {
        std::string h = host;
        if (h.find("radio-browser.info") != std::string::npos && std::find(out.begin(), out.end(), h) == out.end())
          out.push_back(h);
      }
    }
    freeaddrinfo(res);
  }
  if (out.size() > 1) std::rotate(out.begin(), out.begin() + (size_t)(time(nullptr) % (long)out.size()), out.end());   // spread the load
  out.push_back("all.api.radio-browser.info");   // the round-robin name itself (its certificate covers it)
  return out;
}

// ------------------------------------------------------------------------------------------------ favourites
static std::string fav_path(const std::string& dir) { return dir.empty() ? "" : dir + "/favourites.tsv"; }

static std::string clean_field(std::string s) {
  for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

static void load_favs(const std::string& dir, std::vector<Station>& out) {
  out.clear();
  std::string p = fav_path(dir);
  FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "r");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    std::vector<std::string> col;
    char* save = nullptr;
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    size_t a = 0;
    for (;;) {
      size_t b = l.find('\t', a);
      col.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    (void)save;
    if (col.size() < 3 || col[2].empty()) continue;
    Station s;
    s.uuid = col[0];
    s.name = col[1];
    s.url = col[2];
    if (col.size() > 3) s.codec = col[3];
    if (col.size() > 4) s.bitrate = atoi(col[4].c_str());
    if (col.size() > 5) s.country = col[5];
    out.push_back(s);
  }
  fclose(f);
}

static void save_favs(const std::string& dir, const std::vector<Station>& v) {
  std::string p = fav_path(dir);
  if (p.empty()) return;
  std::string tmp = p + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return;
  for (auto& s : v)
    fprintf(f, "%s\t%s\t%s\t%s\t%d\t%s\n", clean_field(s.uuid).c_str(), clean_field(s.name).c_str(),
            clean_field(s.url).c_str(), clean_field(s.codec).c_str(), s.bitrate, clean_field(s.country).c_str());
  fclose(f);
  rename(tmp.c_str(), p.c_str());
}

bool Directory::is_fav(const std::string& uuid) const {
  for (auto& s : favs) if (s.uuid == uuid && !uuid.empty()) return true;
  return false;
}

void Directory::toggle_fav(const Station& st) {
  if (st.url.empty()) return;
  auto it = std::find_if(favs.begin(), favs.end(), [&](const Station& s) {
    return (!st.uuid.empty() && s.uuid == st.uuid) || s.url == st.url;
  });
  if (it != favs.end()) favs.erase(it);
  else favs.insert(favs.begin(), st);
  std::lock_guard<std::mutex> lk(mu_);
  favs_copy_ = favs;
  favs_dirty_ = true;
}

// ------------------------------------------------------------------------------------------------ the worker
void Directory::start(const std::string& data_dir) {
  data_dir_ = data_dir;
  load_favs(data_dir_, favs);
  quit_ = false;
  th_ = std::thread([this] { worker(); });
}

void Directory::stop() {
  quit_ = true;
  if (th_.joinable()) th_.join();
  if (favs_dirty_) save_favs(data_dir_, favs);
}

void Directory::request(const Query& q) {
  std::lock_guard<std::mutex> lk(mu_);
  want_ = q;
  have_want_ = true;
}

void Directory::click(const std::string& uuid) {
  if (uuid.empty()) return;
  std::lock_guard<std::mutex> lk(mu_);
  clicks_.push_back(uuid);
}

bool Directory::take(std::vector<Station>& out, std::string& msg, int& offset, bool& more) {
  std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
  if (!lk.owns_lock() || !done_) return false;
  out.swap(result_);
  result_.clear();
  msg = msg_;
  offset = result_offset_;
  more = more_;
  done_ = false;
  return true;
}

static const int kPageFetch = 64;   // stations per request (eight pages of the list)

void Directory::worker() {
  std::vector<std::string> servers;
  size_t si = 0;
  while (!quit_.load()) {
    Query q;
    bool have = false;
    std::vector<std::string> clicks;
    std::vector<Station> favs_to_save;
    bool save = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (have_want_) { q = want_; have_want_ = false; have = true; }
      clicks.swap(clicks_);
      if (favs_dirty_) { favs_to_save = favs_copy_; favs_dirty_ = false; save = true; }
    }
    if (save) save_favs(data_dir_, favs_to_save);
    if (!have && clicks.empty()) {
      usleep(50000);
      continue;
    }
    if (servers.empty()) servers = find_servers();
    for (auto& u : clicks) {
      std::string body, err;
      long code;
      http_get("https://" + servers[si % servers.size()] + "/json/url/" + urlenc(u), body, code, err);
    }
    if (!have) continue;
    busy = 1;
    std::string path = "/json/stations/search?hidebroken=true&limit=" + std::to_string(kPageFetch) +
                       "&offset=" + std::to_string(q.offset);
    if (!q.order.empty()) path += "&order=" + q.order + (q.reverse ? "&reverse=true" : "&reverse=false");
    if (!q.countrycode.empty()) path += "&countrycode=" + q.countrycode;
    if (q.bitrate_max > 0) path += "&bitrateMax=" + std::to_string(q.bitrate_max);
    std::vector<std::string> tries;
    if (q.kind == 1) {
      tries.push_back(path + "&name=" + urlenc(q.text));
      tries.push_back(path + "&tag=" + urlenc(q.text));   // nothing by name: the text as a style
    } else {
      tries.push_back(q.tag.empty() ? path : path + "&tag=" + urlenc(q.tag));
    }
    std::vector<Station> got;
    std::string msg;
    bool ok = false;
    for (size_t t = 0; t < tries.size() && !ok; ++t) {
      for (size_t attempt = 0; attempt < servers.size() && !quit_.load(); ++attempt) {
        std::string body, err;
        long code = 0;
        const std::string& host = servers[si % servers.size()];
        if (!http_get("https://" + host + tries[t], body, code, err)) {
          msg = err;
          si++;   // the next server
          continue;
        }
        std::vector<std::map<std::string, std::string>> rows;
        if (!json::array_of_objects(body, rows)) {
          msg = "BAD ANSWER";
          si++;
          continue;
        }
        got.clear();
        for (auto& r : rows) {
          Station s;
          s.uuid = r["stationuuid"];
          s.name = r["name"];
          s.url = r["url_resolved"].empty() ? r["url"] : r["url_resolved"];
          s.codec = r["codec"];
          s.country = r["countrycode"];
          s.tags = r["tags"];
          s.bitrate = atoi(r["bitrate"].c_str());
          s.hls = r["hls"] == "1" || r["hls"] == "true";
          while (!s.name.empty() && (s.name.back() == ' ' || s.name.back() == '\t')) s.name.pop_back();
          while (!s.name.empty() && (s.name[0] == ' ' || s.name[0] == '\t')) s.name.erase(0, 1);
          if (s.url.empty() || s.hls || s.name.empty()) continue;   // HLS streams: not played (one URL per stream only)
          got.push_back(s);
        }
        ok = true;
        msg.clear();
        if (got.empty() && t + 1 < tries.size()) ok = false;   // try the next form of the search
        break;
      }
      if (quit_.load()) break;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!have_want_) {   // a newer request supersedes this answer
        result_ = got;
        msg_ = ok ? (got.empty() ? "NO STATIONS FOUND" : "") : "NO CONNECTION TO RADIO-BROWSER.INFO (" + (msg.empty() ? std::string("?") : msg) + ")";
        result_offset_ = q.offset;
        more_ = ok && (int)got.size() >= kPageFetch / 2;
        done_ = true;
      }
    }
    busy = 0;
  }
}

}  // namespace et
