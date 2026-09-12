#include <curl/curl.h>
#include <libxml/parser.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "lakestore/store.hpp"
namespace lakestore {
namespace {
using Headers = std::map<std::string, std::string>;
constexpr size_t kMaxResponse = 512ULL * 1024 * 1024;
struct Response {
  long status = 0;
  std::string bytes;
  Headers headers;
};
std::string lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}
std::string trim(std::string text) {
  auto a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : text.substr(a, b - a + 1);
}
size_t body_callback(char* ptr, size_t size, size_t count, void* target) noexcept {
  try {
    if (size != 0 && count > SIZE_MAX / size) return 0;
    auto n = size * count;
    auto& text = static_cast<Response*>(target)->bytes;
    if (n > kMaxResponse - text.size()) return 0;
    text.append(ptr, n);
    return n;
  } catch (...) {
    return 0;
  }
}
size_t header_callback(char* ptr, size_t size, size_t count, void* target) noexcept {
  try {
    auto n = size * count;
    std::string line(ptr, n);
    auto colon = line.find(':');
    auto& h = static_cast<Response*>(target)->headers;
    if (line.starts_with("HTTP/"))
      h.clear();
    else if (colon != std::string::npos)
      h[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
    return n;
  } catch (...) {
    return 0;
  }
}
std::string escape(const std::string& s) {
  CURL* h = curl_easy_init();
  if (!h) fail(ErrorCode::Fatal, "curl allocation failed");
  char* encoded = curl_easy_escape(h, s.data(), static_cast<int>(s.size()));
  if (!encoded) {
    curl_easy_cleanup(h);
    fail(ErrorCode::Fatal, "URL encoding failed");
  }
  std::string out(encoded);
  curl_free(encoded);
  curl_easy_cleanup(h);
  return out;
}
std::string base64(const unsigned char* bytes, size_t length) {
  std::string out(4 * ((length + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), bytes, static_cast<int>(length));
  return out;
}
std::string decode64(const std::string& text) {
  if (text.empty() || text.size() % 4 != 0 || text.size() > 1024)
    fail(ErrorCode::InvalidArgument, "invalid Azure account key");
  std::string out(text.size(), '\0');
  auto n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                           reinterpret_cast<const unsigned char*>(text.data()),
                           static_cast<int>(text.size()));
  if (n < 0) fail(ErrorCode::InvalidArgument, "invalid Azure account key");
  if (text.back() == '=') --n;
  if (text.size() > 1 && text[text.size() - 2] == '=') --n;
  out.resize(static_cast<size_t>(n));
  return out;
}
uint64_t unsigned_text(const std::string& text) {
  uint64_t n;
  auto [p, e] = std::from_chars(text.data(), text.data() + text.size(), n);
  if (e != std::errc{} || p != text.data() + text.size())
    fail(ErrorCode::Corruption, "invalid cloud object size");
  return n;
}
int64_t date_ms(const std::string& text) {
  auto seconds = curl_getdate(text.c_str(), nullptr);
  if (seconds >= 0) return static_cast<int64_t>(seconds) * 1000;
  std::tm tm{};
  std::istringstream in(text);
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) fail(ErrorCode::Corruption, "invalid object modification time");
  return static_cast<int64_t>(timegm(&tm)) * 1000;
}
ObjectMeta metadata(const Response& r, std::optional<uint64_t> size = {}) {
  auto etag = r.headers.find("etag"), mtime = r.headers.find("last-modified");
  if (etag == r.headers.end()) fail(ErrorCode::Corruption, "cloud response missing ETag");
  uint64_t n = size ? *size : unsigned_text(r.headers.at("content-length"));
  return {n, etag->second, mtime == r.headers.end() ? now_ms() : date_ms(mtime->second)};
}
Error response_error(long status, bool absent = false) {
  if (status == 404) return {ErrorCode::NotFound, "cloud object not found"};
  if (status == 412)
    return {absent ? ErrorCode::AlreadyExists : ErrorCode::PreconditionFailed,
            "cloud write precondition failed"};
  if (status == 408) return {ErrorCode::Timeout, "cloud request timed out"};
  if (status == 409 || status == 429 || status >= 500)
    return {ErrorCode::Transient, "cloud request retryable HTTP " + std::to_string(status)};
  if (status == 416) return {ErrorCode::InvalidArgument, "cloud range outside object"};
  return {ErrorCode::Fatal, "cloud request failed HTTP " + std::to_string(status)};
}
xmlNode* child(xmlNode* parent, const char* name) {
  for (auto p = parent->children; p; p = p->next)
    if (p->type == XML_ELEMENT_NODE && xmlStrEqual(p->name, BAD_CAST name)) return p;
  return nullptr;
}
std::string content(xmlNode* parent, const char* name) {
  auto node = child(parent, name);
  if (!node) fail(ErrorCode::Corruption, "cloud list missing XML field");
  auto bytes = xmlNodeGetContent(node);
  if (!bytes) return {};
  std::string out(reinterpret_cast<const char*>(bytes));
  xmlFree(bytes);
  return out;
}
class CloudStore final : public ObjectStore {
 public:
  explicit CloudStore(CloudConfig config) : c_(std::move(config)) {
    static const bool initialized = [] {
      return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    if (!initialized) fail(ErrorCode::Fatal, "curl initialization failed");
    if (c_.container.empty() || c_.endpoint.empty() || c_.timeout_ms <= 0)
      fail(ErrorCode::InvalidArgument, "cloud endpoint/container required");
    validate_key(c_.container);
    if (c_.container.find('/') != std::string::npos)
      fail(ErrorCode::InvalidArgument, "invalid container name");
    if (!c_.prefix.empty()) {
      if (c_.prefix.back() == '/') c_.prefix.pop_back();
      validate_key(c_.prefix);
      c_.prefix += '/';
    }
    while (c_.endpoint.ends_with('/')) c_.endpoint.pop_back();
    if (!c_.endpoint.starts_with("https://") && !c_.endpoint.starts_with("http://127.0.0.1:") &&
        !c_.endpoint.starts_with("http://localhost:") &&
        !c_.endpoint.starts_with("http://minio:") && !c_.endpoint.starts_with("http://azurite:"))
      fail(ErrorCode::InvalidArgument, "HTTPS required except local emulators");
    for (auto& v : {c_.endpoint, c_.access_key, c_.secret_key, c_.session_token, c_.account,
                    c_.account_key, c_.bearer_token, c_.region})
      if (v.find_first_of("\r\n") != std::string::npos)
        fail(ErrorCode::InvalidArgument, "newline in cloud configuration");
    if (c_.endpoint.find_first_of("?#") != std::string::npos)
      fail(ErrorCode::InvalidArgument, "endpoint must not contain query or fragment");
    if (c_.provider == CloudConfig::Provider::S3 &&
        (c_.access_key.empty() || c_.secret_key.empty()))
      fail(ErrorCode::InvalidArgument, "S3 credentials required");
    if (c_.provider == CloudConfig::Provider::Azure && c_.bearer_token.empty() &&
        (c_.account.empty() || c_.account_key.empty()))
      fail(ErrorCode::InvalidArgument, "Azure credentials required");
  }
  Result<std::string> get(const std::string& k) override {
    return boundary([&]() -> Result<std::string> {
      validate_key(k);
      auto r = request("GET", k);
      if (!r) return r.error();
      if (r.value().status != 200) return response_error(r.value().status);
      return std::move(r.value().bytes);
    });
  }
  Result<std::string> get_range(const std::string& k, uint64_t off, uint64_t len) override {
    return boundary([&]() -> Result<std::string> {
      validate_key(k);
      if (len == 0) {
        auto m = head(k);
        if (!m) return m.error();
        if (off > m.value().size) return Error{ErrorCode::InvalidArgument, "range outside object"};
        return std::string{};
      }
      if (off > UINT64_MAX - (len - 1) || len > kMaxResponse)
        return Error{ErrorCode::InvalidArgument, "invalid range"};
      auto r = request(
          "GET", k, {},
          {{"range", "bytes=" + std::to_string(off) + "-" + std::to_string(off + len - 1)}});
      if (!r) return r.error();
      if (r.value().status != 206) return response_error(r.value().status);
      if (r.value().bytes.size() != len)
        return Error{ErrorCode::InvalidArgument, "short ranged read"};
      return std::move(r.value().bytes);
    });
  }
  Result<ObjectMeta> head(const std::string& k) override {
    return boundary([&]() -> Result<ObjectMeta> {
      validate_key(k);
      auto r = request("HEAD", k);
      if (!r) return r.error();
      if (r.value().status != 200) return response_error(r.value().status);
      return metadata(r.value());
    });
  }
  Result<ObjectMeta> put(const std::string& k, std::string_view v) override {
    return write(k, v, {});
  }
  Result<ObjectMeta> put_if_absent(const std::string& k, std::string_view v) override {
    return write(k, v, {{"if-none-match", "*"}});
  }
  Result<ObjectMeta> put_if_match(const std::string& k, std::string_view v,
                                  const std::string& e) override {
    if (e.empty() || e.find_first_of("\r\n") != std::string::npos)
      return Error{ErrorCode::InvalidArgument, "invalid ETag"};
    return write(k, v, {{"if-match", e}});
  }
  Status erase(const std::string& k) override {
    return boundary([&]() -> Status {
      validate_key(k);
      auto r = request("DELETE", k);
      if (!r) return r.error();
      if (r.value().status == 204 || r.value().status == 202 || r.value().status == 404)
        return ok();
      return response_error(r.value().status);
    });
  }
  Result<std::vector<KeyInfo>> list(const std::string& prefix) override {
    return boundary([&]() -> Result<std::vector<KeyInfo>> {
      if (!prefix.empty())
        validate_key(prefix.ends_with('/') ? prefix.substr(0, prefix.size() - 1) : prefix);
      std::vector<KeyInfo> out;
      std::string token;
      std::set<std::string> tokens;
      do {
        Headers query{{"prefix", c_.prefix + prefix}};
        bool s3 = c_.provider == CloudConfig::Provider::S3;
        if (s3) {
          query["list-type"] = "2";
          query["max-keys"] = "1000";
          if (!token.empty()) query["continuation-token"] = token;
        } else {
          query["restype"] = "container";
          query["comp"] = "list";
          query["maxresults"] = "1000";
          if (!token.empty()) query["marker"] = token;
        }
        auto response = request("GET", "", {}, {}, query);
        if (!response) return response.error();
        auto& r = response.value();
        if (r.status != 200) return response_error(r.status);
        if (r.bytes.size() > static_cast<size_t>(INT_MAX))
          fail(ErrorCode::Corruption, "oversized list XML");
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
            xmlReadMemory(r.bytes.data(), static_cast<int>(r.bytes.size()), nullptr, nullptr,
                          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
            xmlFreeDoc);
        if (!doc || doc->intSubset || doc->extSubset)
          fail(ErrorCode::Corruption, "invalid cloud list XML");
        auto root = xmlDocGetRootElement(doc.get());
        if (!root) fail(ErrorCode::Corruption, "empty list XML");
        auto parent = s3 ? root : child(root, "Blobs");
        if (!parent) fail(ErrorCode::Corruption, "missing list entries");
        for (auto node = parent->children; node; node = node->next) {
          if (node->type != XML_ELEMENT_NODE ||
              !xmlStrEqual(node->name, BAD_CAST(s3 ? "Contents" : "Blob")))
            continue;
          auto key = content(node, s3 ? "Key" : "Name");
          if (!key.starts_with(c_.prefix + prefix))
            fail(ErrorCode::Corruption, "list key outside prefix");
          auto props = s3 ? node : child(node, "Properties");
          if (!props) fail(ErrorCode::Corruption, "missing blob properties");
          auto etag = content(props, s3 ? "ETag" : "Etag");
          // Azure list XML can omit the quotes present in the HTTP ETag header.
          if (!s3 && !etag.starts_with('"')) etag = "\"" + etag + "\"";
          out.push_back({key.substr(c_.prefix.size()),
                         {unsigned_text(content(props, s3 ? "Size" : "Content-Length")), etag,
                          date_ms(content(props, s3 ? "LastModified" : "Last-Modified"))}});
        }
        token =
            s3 ? (content(root, "IsTruncated") == "true" ? content(root, "NextContinuationToken")
                                                         : "")
               : content(root, "NextMarker");
        if (!token.empty() && !tokens.insert(token).second)
          fail(ErrorCode::Corruption, "repeated list continuation token");
      } while (!token.empty());
      std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.key < b.key; });
      return out;
    });
  }
  Status initialize() {
    return boundary([&]() -> Status {
      Headers query;
      std::string body;
      if (c_.provider == CloudConfig::Provider::Azure)
        query["restype"] = "container";
      else if (c_.region != "us-east-1")
        body =
            "<CreateBucketConfiguration "
            "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><LocationConstraint>" +
            c_.region + "</LocationConstraint></CreateBucketConfiguration>";
      auto r = request("PUT", "", body, {}, query);
      if (!r) return r.error();
      if (r.value().status == 200 || r.value().status == 201) return ok();
      // Existing namespaces are checked with a read rather than treating all 409s as success.
      if (r.value().status == 409) {
        auto check = list("");
        if (check) return ok();
        return check.error();
      }
      return response_error(r.value().status);
    });
  }

 private:
  template <class F>
  auto boundary(F fn) -> decltype(fn()) {
    try {
      return fn();
    } catch (const Failure& e) {
      return e.error();
    } catch (const std::exception&) {
      return Error{ErrorCode::Fatal, "cloud adapter exception"};
    }
  }
  Result<ObjectMeta> write(const std::string& k, std::string_view bytes, Headers headers) {
    return boundary([&]() -> Result<ObjectMeta> {
      validate_key(k);
      if (bytes.size() > kMaxResponse)
        return Error{ErrorCode::InvalidArgument, "object exceeds 512 MiB"};
      bool absent = headers.contains("if-none-match");
      bool matched = headers.contains("if-match");
      auto r = request("PUT", k, bytes, std::move(headers));
      if (!r) return r.error();
      if (r.value().status != 200 && r.value().status != 201) {
        const auto error = r.value().headers.find("x-ms-error-code");
        if (absent && r.value().status == 409 && error != r.value().headers.end() &&
            error->second == "BlobAlreadyExists")
          return Error{ErrorCode::AlreadyExists, "Azure blob already exists"};
        if (r.value().status == 404 && matched)
          return Error{ErrorCode::PreconditionFailed, "conditional target missing"};
        return response_error(r.value().status, absent);
      }
      return metadata(r.value(), bytes.size());
    });
  }
  Result<Response> request(const std::string& method, const std::string& key,
                           std::string_view bytes = {}, Headers headers = {}, Headers query = {}) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), curl_easy_cleanup);
    if (!h) return Error{ErrorCode::Fatal, "curl allocation failed"};
    auto url = c_.endpoint + "/" + c_.container;
    if (!key.empty()) url += "/" + c_.prefix + key;
    auto path_pos = url.find('/', url.find("://") + 3);
    auto path = url.substr(path_pos);
    for (auto it = query.begin(); it != query.end(); ++it)
      url += (it == query.begin() ? "?" : "&") + escape(it->first) + "=" + escape(it->second);
    if (method == "PUT") {
      headers["content-type"] = "application/octet-stream";
      headers["content-length"] = std::to_string(bytes.size());
    }
    if (c_.provider == CloudConfig::Provider::Azure) {
      auto now = std::time(nullptr);
      std::tm tm{};
      gmtime_r(&now, &tm);
      char date[64];
      std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
      headers["x-ms-date"] = date;
      headers["x-ms-version"] = "2023-11-03";
      if (method == "PUT" && !key.empty()) headers["x-ms-blob-type"] = "BlockBlob";
      if (!c_.bearer_token.empty())
        headers["authorization"] = "Bearer " + c_.bearer_token;
      else {
        std::string sign = method + "\n";
        for (auto name : {"content-encoding", "content-language", "content-length", "content-md5",
                          "content-type", "date", "if-modified-since", "if-match", "if-none-match",
                          "if-unmodified-since", "range"}) {
          auto it = headers.find(name);
          if (it != headers.end() &&
              !(std::string_view(name) == "content-length" && it->second == "0"))
            sign += it->second;
          sign += '\n';
        }
        for (auto& [name, value] : headers)
          if (name.starts_with("x-ms-")) sign += name + ":" + value + "\n";
        sign += "/" + c_.account + path;
        for (auto& [name, value] : query) sign += "\n" + lower(name) + ":" + value;
        auto secret = decode64(c_.account_key);
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                  reinterpret_cast<const unsigned char*>(sign.data()), sign.size(), digest,
                  &length))
          return Error{ErrorCode::Fatal, "Azure signing failed"};
        headers["authorization"] = "SharedKey " + c_.account + ":" + base64(digest, length);
      }
    }
    Response response;
    curl_slist* raw = nullptr;
    for (auto& [name, value] : headers) {
      auto next = curl_slist_append(raw, (name + ": " + value).c_str());
      if (!next) {
        curl_slist_free_all(raw);
        return Error{ErrorCode::Fatal, "curl header allocation failed"};
      }
      raw = next;
    }
    auto next = curl_slist_append(raw, "Expect:");
    if (!next) {
      curl_slist_free_all(raw);
      return Error{ErrorCode::Fatal, "curl header allocation failed"};
    }
    raw = next;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> list(raw, curl_slist_free_all);
    curl_easy_setopt(h.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(h.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, list.get());
    curl_easy_setopt(h.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h.get(), CURLOPT_TIMEOUT_MS, c_.timeout_ms);
    curl_easy_setopt(h.get(), CURLOPT_CONNECTTIMEOUT_MS, std::min(5000L, c_.timeout_ms));
    curl_easy_setopt(h.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h.get(), CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(h.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(h.get(), CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(h.get(), CURLOPT_HEADERDATA, &response);
    if (method == "HEAD") curl_easy_setopt(h.get(), CURLOPT_NOBODY, 1L);
    if (method == "PUT") {
      curl_easy_setopt(h.get(), CURLOPT_POSTFIELDS, bytes.empty() ? "" : bytes.data());
      curl_easy_setopt(h.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bytes.size()));
    }
    std::string auth, signing;
    if (c_.provider == CloudConfig::Provider::S3) {
      auth = c_.access_key + ":" + c_.secret_key;
      signing = "aws:amz:" + c_.region + ":s3";
      if (curl_easy_setopt(h.get(), CURLOPT_AWS_SIGV4, signing.c_str()) != CURLE_OK)
        return Error{ErrorCode::Fatal, "libcurl lacks AWS SigV4 support"};
      curl_easy_setopt(h.get(), CURLOPT_USERPWD, auth.c_str());
      if (!c_.session_token.empty()) {
        auto token_header = "x-amz-security-token: " + c_.session_token;
        auto updated = curl_slist_append(list.get(), token_header.c_str());
        if (!updated) return Error{ErrorCode::Fatal, "header allocation failed"};
        list.release();
        list.reset(updated);
        curl_easy_setopt(h.get(), CURLOPT_HTTPHEADER, list.get());
      }
    }
    auto code = curl_easy_perform(h.get());
    if (code != CURLE_OK) {
      if (code == CURLE_OPERATION_TIMEDOUT || code == CURLE_SEND_ERROR ||
          code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING || code == CURLE_PARTIAL_FILE)
        return Error{ErrorCode::Timeout, "cloud transfer outcome uncertain"};
      if (code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST)
        return Error{ErrorCode::Transient, "cloud connection unavailable"};
      return Error{ErrorCode::Fatal, "cloud transport failed"};
    }
    curl_easy_getinfo(h.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
  }
  CloudConfig c_;
};
}  // namespace
std::shared_ptr<ObjectStore> cloud_store(CloudConfig c) {
  return std::make_shared<CloudStore>(std::move(c));
}
Status initialize_cloud(CloudConfig c) { return CloudStore(std::move(c)).initialize(); }
}  // namespace lakestore
