// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#ifndef SRC_RUNTIME_CONFIG_HPP_
#define SRC_RUNTIME_CONFIG_HPP_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "absl/status/status.h"

namespace runtime_config {

enum class Type { Bool, Int, UInt16, String };

// Schema entry for one user-overridable flag.  The schema is the single
// source of truth for: which flags can be overridden via the config file,
// how the admin UI renders them, and how values are validated.
struct Entry {
  const char* name;
  Type        type;
  const char* description;
  const char* group;        ///< UI grouping label
};

// Returns the (statically-defined) schema.  Order is preserved for the
// admin UI rendering.
const std::vector<Entry>& Schema();

// Reads @p path (a flat JSON object of string values) and, for every
// non-empty value matching a schema entry, calls absl::FindCommandLineFlag
// + ParseFrom to override the flag.  Missing file or empty string values
// are no-ops.  Returns the number of overrides applied.  Errors are
// logged but non-fatal -- a misconfigured key never blocks startup.
int LoadFromFile(const std::string& path);

// Reads @p path and returns the raw string value for each schema key.
// Missing file -> map with empty values for every schema key.
std::map<std::string, std::string> ReadFromFile(const std::string& path);

// Writes @p values as a JSON object to @p path atomically (tmp + rename).
// Keys not present in @p values are written as empty strings; unknown
// keys (not in the schema) are dropped.
absl::Status WriteToFile(const std::string& path,
                         const std::map<std::string, std::string>& values);

// @p type lower-cased and without surrounding spaces, the form object
// types are compared in.  human, the ONVIF name, becomes person.
std::string NormalizeObjectType(const std::string& type);

// Checks one object type: person (or human), vehicle, animal or package,
// plus motion when @p allow_motion (default_object_type only).  Case and
// surrounding spaces are ignored.  The error message names the valid values.
absl::Status ValidateObjectType(const std::string& type, bool allow_motion);

// Checks @p value against the values setting @p name accepts beyond what
// its flag type allows: default_object_type takes one object type, and
// camera_object_types takes ip=type pairs.  OK for every other setting
// and for an empty value, which clears the setting.
absl::Status ValidateValue(const std::string& name, const std::string& value);

// Calls @p fn(ip, value) for each ip=value pair of the comma-separated
// @p list, with surrounding spaces removed from both.  Pairs without '='
// or with an empty ip are skipped.
void ForEachIpValue(
    const std::string& list,
    const std::function<void(const std::string&, const std::string&)>& fn);

}  // namespace runtime_config

#endif  // SRC_RUNTIME_CONFIG_HPP_
