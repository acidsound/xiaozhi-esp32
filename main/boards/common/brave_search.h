#ifndef BRAVE_SEARCH_H
#define BRAVE_SEARCH_H

#include <cJSON.h>
#include <string>

namespace brave_search {

cJSON* GetConfigStatus();
cJSON* Search(const std::string& query, int max_results, const std::string& mode);

} // namespace brave_search

#endif // BRAVE_SEARCH_H
