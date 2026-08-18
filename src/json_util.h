#pragma once

#include <string>
#include <vector>

namespace agstel {

// Escapes a string for embedding inside a JSON string literal (no surrounding quotes).
std::string JsonEscape(const std::string& s);

// Naive extraction of an integer field ("key":123) from a small JSON body.
// Returns fallback if not found.
long ExtractLong(const std::string& json, const std::string& key, long fallback);

// Naive extraction of a JSON array of integers ("key":[1,2,3]).
// Returns empty vector if not found.
std::vector<long> ExtractLongArray(const std::string& json, const std::string& key);

}  // namespace agstel
