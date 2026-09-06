// Real-service test cleanup. Restricted to a random validation prefix, never a whole container.
#include <cstdlib>
#include <iostream>
#include <regex>

#include "lakestore/store.hpp"
using namespace lakestore;
namespace {
std::string env(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 3 || !std::regex_match(argv[2], std::regex("validation-[a-f0-9]{32}")))
      fail(ErrorCode::InvalidArgument, "cleanup requires a random validation prefix");
    CloudConfig c;
    c.provider = CloudConfig::Provider::Azure;
    c.container = argv[1];
    c.prefix = argv[2];
    c.account = env("AZURE_STORAGE_ACCOUNT");
    c.account_key = env("AZURE_STORAGE_KEY");
    c.bearer_token = env("AZURE_STORAGE_BEARER_TOKEN");
    c.endpoint = "https://" + c.account + ".blob.core.windows.net";
    auto store = cloud_store(c);
    auto objects = store->list("").value();
    for (const auto& object : objects) store->erase(object.key).value();
    std::cout << objects.size() << " validation objects removed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
