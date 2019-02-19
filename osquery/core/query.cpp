/**
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed in accordance with the terms specified in
 *  the LICENSE file found in the root directory of this source tree.
 */

#include <algorithm>
#include <string>
#include <vector>

#include <osquery/database.h>
#include <osquery/flags.h>
#include <osquery/logger.h>
#include <osquery/query.h>

#include <osquery/utils/json/json.h>

namespace rj = rapidjson;

namespace osquery {

DECLARE_bool(decorations_top_level);

uint64_t Query::getPreviousEpoch() const {
  uint64_t epoch = 0;
  std::string raw;
  auto status = getDatabaseValue(kQueries, name_ + "epoch", raw);
  if (status.ok()) {
    epoch = std::stoull(raw);
  }
  return epoch;
}

uint64_t Query::getQueryCounter(bool new_query) const {
  uint64_t counter = 0;
  if (new_query) {
    return counter;
  }

  std::string raw;
  auto status = getDatabaseValue(kQueries, name_ + "counter", raw);
  if (status.ok()) {
    counter = std::stoull(raw) + 1;
  }
  return counter;
}

Status Query::getPreviousQueryResults(QueryDataSet& results) const {
  std::string raw;
  auto status = getDatabaseValue(kQueries, name_, raw);
  if (!status.ok()) {
    return status;
  }

  status = deserializeQueryDataJSON(raw, results);
  if (!status.ok()) {
    return status;
  }
  return Status(0, "OK");
}

std::vector<std::string> Query::getStoredQueryNames() {
  std::vector<std::string> results;
  scanDatabaseKeys(kQueries, results);
  return results;
}

bool Query::isQueryNameInDatabase() const {
  auto names = Query::getStoredQueryNames();
  return std::find(names.begin(), names.end(), name_) != names.end();
}

static inline void saveQuery(const std::string& name,
                             const std::string& query) {
  setDatabaseValue(kQueries, "query." + name, query);
}

bool Query::isNewQuery() const {
  std::string query;
  getDatabaseValue(kQueries, "query." + name_, query);
  return (query != query_);
}

Status Query::addNewResults(QueryData qd,
                            const uint64_t epoch,
                            uint64_t& counter) const {
  DiffResults dr;
  return addNewResults(std::move(qd), epoch, counter, dr, false);
}

Status Query::addNewResults(QueryData current_qd,
                            const uint64_t current_epoch,
                            uint64_t& counter,
                            DiffResults& dr,
                            bool calculate_diff) const {
  // The current results are 'fresh' when not calculating a differential.
  bool fresh_results = !calculate_diff;
  bool new_query = false;
  if (!isQueryNameInDatabase()) {
    // This is the first encounter of the scheduled query.
    fresh_results = true;
    LOG(INFO) << "Storing initial results for new scheduled query: " << name_;
    saveQuery(name_, query_);
  } else if (getPreviousEpoch() != current_epoch) {
    fresh_results = true;
    LOG(INFO) << "New Epoch " << current_epoch << " for scheduled query "
              << name_;
  } else if (isNewQuery()) {
    // This query is 'new' in that the previous results may be invalid.
    new_query = true;
    LOG(INFO) << "Scheduled query has been updated: " + name_;
    saveQuery(name_, query_);
  }

  // Use a 'target' avoid copying the query data when serializing and saving.
  // If a differential is requested and needed the target remains the original
  // query data, otherwise the content is moved to the differential's added set.
  const auto* target_gd = &current_qd;
  bool update_db = true;
  if (!fresh_results && calculate_diff) {
    // Get the rows from the last run of this query name.
    QueryDataSet previous_qd;
    auto status = getPreviousQueryResults(previous_qd);
    if (!status.ok()) {
      return status;
    }

    // Calculate the differential between previous and current query results.
    dr = diff(previous_qd, current_qd);

    update_db = (!dr.added.empty() || !dr.removed.empty());
  } else {
    dr.added = std::move(current_qd);
    target_gd = &dr.added;
  }

  counter = getQueryCounter(fresh_results || new_query);
  auto status =
      setDatabaseValue(kQueries, name_ + "counter", std::to_string(counter));
  if (!status.ok()) {
    return status;
  }

  if (update_db) {
    // Replace the "previous" query data with the current.
    std::string json;
    status = serializeQueryDataJSON(*target_gd, json);
    if (!status.ok()) {
      return status;
    }

    status = setDatabaseValue(kQueries, name_, json);
    if (!status.ok()) {
      return status;
    }

    status = setDatabaseValue(
        kQueries, name_ + "epoch", std::to_string(current_epoch));
    if (!status.ok()) {
      return status;
    }
  }
  return Status(0, "OK");
}


Status deserializeDiffResults(const rj::Value& doc, DiffResults& dr) {
  if (!doc.IsObject()) {
    return Status(1);
  }

  if (doc.HasMember("removed")) {
    auto status = deserializeQueryData(doc["removed"], dr.removed);
    if (!status.ok()) {
      return status;
    }
  }

  if (doc.HasMember("added")) {
    auto status = deserializeQueryData(doc["added"], dr.added);
    if (!status.ok()) {
      return status;
    }
  }
  return Status();
}

inline void addLegacyFieldsAndDecorations(const QueryLogItem& item,
                                          JSON& doc,
                                          rj::Document& obj) {
  // Apply legacy fields.
  doc.addRef("name", item.name, obj);
  doc.addRef("hostIdentifier", item.identifier, obj);
  doc.addRef("calendarTime", item.calendar_time, obj);
  doc.add("unixTime", item.time, obj);
  doc.add("epoch", static_cast<size_t>(item.epoch), obj);
  doc.add("counter", static_cast<size_t>(item.counter), obj);

  // Append the decorations.
  if (!item.decorations.empty()) {
    auto dec_obj = doc.getObject();
    auto target_obj = std::ref(dec_obj);
    if (FLAGS_decorations_top_level) {
      target_obj = std::ref(obj);
    }
    for (const auto& name : item.decorations) {
      doc.addRef(name.first, name.second, target_obj);
    }
    if (!FLAGS_decorations_top_level) {
      doc.add("decorations", dec_obj, obj);
    }
  }
}

inline void getLegacyFieldsAndDecorations(const JSON& doc, QueryLogItem& item) {
  if (doc.doc().HasMember("decorations")) {
    if (doc.doc()["decorations"].IsObject()) {
      for (const auto& i : doc.doc()["decorations"].GetObject()) {
        item.decorations[i.name.GetString()] = i.value.GetString();
      }
    }
  }

  item.name = doc.doc()["name"].GetString();
  item.identifier = doc.doc()["hostIdentifier"].GetString();
  item.calendar_time = doc.doc()["calendarTime"].GetString();
  item.time = doc.doc()["unixTime"].GetUint64();
}

Status serializeQueryLogItem(const QueryLogItem& item, JSON& doc) {
  if (item.results.added.size() > 0 || item.results.removed.size() > 0) {
    auto obj = doc.getObject();
    auto status = serializeDiffResults(item.results, item.columns, doc, obj);
    if (!status.ok()) {
      return status;
    }

    doc.add("diffResults", obj);
  } else {
    auto arr = doc.getArray();
    auto status =
        serializeQueryData(item.snapshot_results, item.columns, doc, arr);
    if (!status.ok()) {
      return status;
    }

    doc.add("snapshot", arr);
    doc.addRef("action", "snapshot");
  }

  addLegacyFieldsAndDecorations(item, doc, doc.doc());
  return Status();
}

Status serializeEvent(const QueryLogItem& item,
                      const rj::Value& event_obj,
                      JSON& doc,
                      rj::Document& obj) {
  addLegacyFieldsAndDecorations(item, doc, obj);

  auto columns_obj = doc.getObject();
  for (const auto& i : event_obj.GetObject()) {
    // Yield results as a "columns." map to avoid namespace collisions.
    doc.addCopy(i.name.GetString(), i.value.GetString(), columns_obj);
  }

  doc.add("columns", columns_obj, obj);
  return Status(0, "OK");
}

Status serializeQueryLogItemAsEvents(const QueryLogItem& item, JSON& doc) {
  auto temp_doc = JSON::newObject();
  if (!item.results.added.empty() || !item.results.removed.empty()) {
    auto status = serializeDiffResults(
        item.results, item.columns, temp_doc, temp_doc.doc());
    if (!status.ok()) {
      return status;
    }
  } else if (!item.snapshot_results.empty()) {
    auto arr = doc.getArray();
    auto status = serializeQueryData(item.snapshot_results, {}, temp_doc, arr);
    if (!status.ok()) {
      return status;
    }
    temp_doc.add("snapshot", arr);
  } else {
    // This error case may also be represented in serializeQueryLogItem.
    return Status(1, "No differential or snapshot results");
  }

  for (auto& action : temp_doc.doc().GetObject()) {
    for (auto& row : action.value.GetArray()) {
      auto obj = doc.getObject();
      serializeEvent(item, row, doc, obj);
      doc.addCopy("action", action.name.GetString(), obj);
      doc.push(obj);
    }
  }
  return Status();
}

Status serializeQueryLogItemJSON(const QueryLogItem& item, std::string& json) {
  auto doc = JSON::newObject();
  auto status = serializeQueryLogItem(item, doc);
  if (!status.ok()) {
    return status;
  }

  return doc.toString(json);
}

Status deserializeQueryLogItem(const JSON& doc, QueryLogItem& item) {
  if (!doc.doc().IsObject()) {
    return Status(1);
  }

  if (doc.doc().HasMember("diffResults")) {
    auto status =
        deserializeDiffResults(doc.doc()["diffResults"], item.results);
    if (!status.ok()) {
      return status;
    }
  } else if (doc.doc().HasMember("snapshot")) {
    auto status =
        deserializeQueryData(doc.doc()["snapshot"], item.snapshot_results);
    if (!status.ok()) {
      return status;
    }
  }

  getLegacyFieldsAndDecorations(doc, item);
  return Status();
}

Status deserializeQueryLogItemJSON(const std::string& json,
                                   QueryLogItem& item) {
  auto doc = JSON::newObject();
  if (!doc.fromString(json) || !doc.doc().IsObject()) {
    return Status(1, "Cannot deserialize JSON");
  }
  return deserializeQueryLogItem(doc, item);
}

Status serializeQueryLogItemAsEventsJSON(const QueryLogItem& item,
                                         std::vector<std::string>& items) {
  auto doc = JSON::newArray();
  auto status = serializeQueryLogItemAsEvents(item, doc);
  if (!status.ok()) {
    return status;
  }

  // return doc.toString()
  for (auto& event : doc.doc().GetArray()) {
    rj::StringBuffer sb;
    rj::Writer<rj::StringBuffer> writer(sb);
    event.Accept(writer);
    items.push_back(sb.GetString());
  }
  return Status();
}

}
