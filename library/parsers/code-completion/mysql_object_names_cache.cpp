/* 
 * Copyright (c) 2012, 2015, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include <glib.h>

#include "base/string_utilities.h"
#include "base/log.h"
#include "base/file_utilities.h"
#include "base/sqlstring.h"
#include "base/threading.h"
#include "base/notifications.h"

#include "mysql_object_names_cache.h"

DEFAULT_LOG_DOMAIN("MySQL Name Cache");

// The cache automatically loads objects once on startup (for the main objects like schema names)
// and when queried (for the others). After that no fetch is performed anymore until an explicit
// refresh is requested by the application (via any of the refresh* functions).

//--------------------------------------------------------------------------------------------------

MySQLObjectNamesCache::MySQLObjectNamesCache(ObjectQueryCallback getValues, std::function<void(bool)> feedback)
  : _cacheWworking(1)
{
  _refreshThread = nullptr;
  _shutdown = false;
  _feedback = feedback;
  _getValues = getValues;

  // Start loading top level objects.
  addPendingRefresh(RefreshTask::RefreshSchemas);
  addPendingRefresh(RefreshTask::RefreshUDFs);
  addPendingRefresh(RefreshTask::RefreshLogfileGroups);
  addPendingRefresh(RefreshTask::RefreshTableSpaces);
  addPendingRefresh(RefreshTask::RefreshVariables);
  addPendingRefresh(RefreshTask::RefreshEngines);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::shutdown()
{
  {
    base::RecMutexLock pending_lock(_pendingMutex);
    _shutdown = true;

    _pendingTasks.clear();
    _feedback = nullptr;
  }

  if (_refreshThread != nullptr)
  {
    logDebug2("Waiting for worker thread to finish...\n");
    g_thread_join(_refreshThread);
    _refreshThread = NULL;
    logDebug2("Worker thread finished.\n");
  }
}

//--------------------------------------------------------------------------------------------------

MySQLObjectNamesCache::~MySQLObjectNamesCache()
{
  g_assert(_shutdown);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingSchemaNames(const std::string &prefix)
{
  return getMatchingObjects("schemas", "", "",  prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingTableNames(const std::string &schema, const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);

  return getMatchingObjects("tables", schema, "", prefix, RetrieveWithSchemaQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingViewNames(const std::string &schema, const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);

  return getMatchingObjects("views", schema, "", prefix, RetrieveWithSchemaQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingColumnNames(const std::string &schema,
  const std::string &table, const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);
  
  return getMatchingObjects("columns", schema, table, prefix, RetrieveWithFullQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingProcedureNames(const std::string &schema,
  const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);
  
  return getMatchingObjects("procedures", schema, "", prefix, RetrieveWithSchemaQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingFunctionNames(const std::string &schema,
  const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);

  return getMatchingObjects("functions", schema, "", prefix, RetrieveWithSchemaQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingTriggerNames(const std::string &schema,
  const std::string &table, const std::string &prefix)
{
  loadSchemaObjectsIfNeeded(schema);

  return getMatchingObjects("triggers", schema, table, prefix, RetrieveWithFullQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingUdfNames(const std::string &prefix)
{
  return getMatchingObjects("udfs", "", "", prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingVariables(const std::string &prefix)
{
  // System variables names are cached at startup as their existence/names will never change.
  return getMatchingObjects("variables", "", "", prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingEngines(const std::string &prefix)
{
  // Engines are cached at startup as they will never change (as long as we are connected).
  return getMatchingObjects("engines", "", "", prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingLogfileGroups(const std::string &prefix)
{
  addPendingRefresh(RefreshTask::RefreshLogfileGroups);

  return getMatchingObjects("logfile_groups", "", "", prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

std::vector<std::string> MySQLObjectNamesCache::getMatchingTablespaces(const std::string &prefix)
{
  addPendingRefresh(RefreshTask::RefreshTableSpaces);

  return getMatchingObjects("tablespaces", "", "", prefix, RetrieveWithNoQualifier);
}

//--------------------------------------------------------------------------------------------------

/**
 * Core object retrieval function.
 */
std::vector<std::string> MySQLObjectNamesCache::getMatchingObjects(const std::string &cache,
  const std::string &schema, const std::string &table, const std::string &prefix, RetrievalType type)
{
  if (_shutdown)
    return {};

  std::vector<std::string> items;

  base::RecMutexLock lock(_cacheLock);
  switch (type)
  {
  case RetrieveWithNoQualifier:
    for (auto &entry : _topLevelCache[cache])
    {
      if (base::hasPrefix(entry, prefix))
        items.push_back(entry);
    }
    break;

    case RetrieveWithSchemaQualifier:
    {
      if (schema.empty()) // Match entries from all schemas.
      {
        for (auto &entry : _schemaObjectsCache)
        {
          if (entry.first.second == cache) // Consider only the object type given.
          {
            for (auto objectEntry : entry.second)
            {
              if (base::hasPrefix(objectEntry, prefix))
                items.push_back(objectEntry);
            }
          }
        }
      }
      else
      {
        // Most common case, hence optimized.
        for (auto &entry : _schemaObjectsCache[{schema, cache}])
        {
          if (base::hasPrefix(entry, prefix))
            items.push_back(entry);
        }
      }

      break;
    }

  default: // RetrieveWithFullQualifier, for columns. Cache should be "columns" even though we don't use that here yet.
    {
      CacheObjectType type = (cache == "triggers") ? TriggersCacheType : ColumnsCacheType;
      if (schema.empty() && table.empty())
      {
        // Columns/triggers from all tables in all schemas.
        for (auto &schemaEntry : _tableObjectsCache)
        {
          for (auto &tableEntry : schemaEntry.second)
          {
            for (auto &objectEntry : tableEntry.second[type])
            {
              if (base::hasPrefix(objectEntry, prefix))
                items.push_back(objectEntry);
            }
          }
        }
        break;
      }

      if (!schema.empty() && !table.empty())
      {
        // Objects only from a specific table in a specific schema.
        // This is the most common case, hence optimized.
        for (auto &objectEntry : _tableObjectsCache[schema][table][type])
        {
          if (base::hasPrefix(objectEntry, prefix))
            items.push_back(objectEntry);
        }
        break;
      }

      if (!schema.empty())
      {
        // Objects from all tables in a specific schema.
        for (auto &schemaEntry : _tableObjectsCache[schema])
        {
          for (auto objectEntry : schemaEntry.second[type])
          {
            if (base::hasPrefix(objectEntry, prefix))
              items.push_back(objectEntry);
          }
        }
        break;
      }

      // Objects from all schemas, using the same table in all of them.
      for (auto &schemaEntry : _tableObjectsCache)
      {
        for (auto &objectEntry : schemaEntry.second[table][type])
        {
          if (base::hasPrefix(objectEntry, prefix))
            items.push_back(objectEntry);
        }
      }

      break;
    }
  }
  return items;
}

//--------------------------------------------------------------------------------------------------

/**
 * Update all schema names. Used by code outside of this class.
 */
void MySQLObjectNamesCache::refreshSchemaCache()
{
  addPendingRefresh(RefreshTask::RefreshSchemas);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::refreshColumns(const std::string &schema, const std::string &table)
{
  addPendingRefresh(RefreshTask::RefreshColumns, schema, table);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::refreshTriggers(const std::string &schema, const std::string &table)
{
  addPendingRefresh(RefreshTask::RefreshTriggers, schema, table);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::refreshTablespaces()
{
  addPendingRefresh(RefreshTask::RefreshTableSpaces);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::refreshLogfileGroups()
{
  addPendingRefresh(RefreshTask::RefreshLogfileGroups);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::refreshThread()
{
  logDebug2("entering worker thread\n");

  while (!_shutdown)
  {
    try
    {
      RefreshTask task;
      if (!getPendingRefresh(task)) // If there's nothing more to do end the thread.
        break;

      if (_shutdown)
        break;

      switch (task.type)
      {
        case RefreshTask::RefreshSchemas:
          doRefreshSchemas();
          break;

        case RefreshTask::RefreshTables:
          doRefreshTables(task.schemaName);
          break;

        case RefreshTask::RefreshViews:
          doRefreshViews(task.schemaName);
          break;

        case RefreshTask::RefreshProcedures:
          doRefreshProcedures(task.schemaName);
          break;

        case RefreshTask::RefreshFunctions:
          doRefreshFunctions(task.schemaName);
          break;

        case RefreshTask::RefreshColumns:
          doRefreshColumns(task.schemaName, task.tableName);
          break;

        case RefreshTask::RefreshTriggers:
          doRefreshTriggers(task.schemaName, task.tableName);
          break;

        case RefreshTask::RefreshUDFs:
          doRefreshUdfs();
          break;

        case RefreshTask::RefreshVariables:
          doRefreshVariables();
          break;

        case RefreshTask::RefreshEngines:
          doRefreshEngines();
          break;

        case RefreshTask::RefreshLogfileGroups:
          doRefreshLogfileGroups();
          break;

        case RefreshTask::RefreshTableSpaces:
          doRefreshTablespaces();
          break;
      }
    }
    catch (std::exception &exc)
    {
      logError("Exception while running refresh task: %s\n", exc.what());
    }
  }

  // Signal the main thread that the worker thread is (about to be) gone.
  _cacheWworking.post();

  if (_feedback && !_shutdown)
    _feedback(false);

  logDebug2("leaving worker thread\n");
}

//--------------------------------------------------------------------------------------------------

/**
 * Checks if the objects from the given schema were loaded already.
 * If not, the loading is triggered.
 */
bool MySQLObjectNamesCache::loadSchemaObjectsIfNeeded(const std::string &schema)
{
  if (schema.empty() || _shutdown)
    return false;

  // Check for schema sentinel (schema + empty object name tuple).
  if (_schemaObjectsCache.find({schema, ""}) != _schemaObjectsCache.end())
  {
    logDebug3("Request to load schema objects for %s, but objects are already cached\n", schema.c_str());
    return false;
  }

  // Add tasks to load various schema objects.
  logDebug3("Request to load schema objects for %s\n", schema.c_str());

  // Add schema sentinel as marker that we loaded the schema already. For consistency also add
  // the given schema to the top level cache, if not there yet.
  base::RecMutexLock lock(_cacheLock);
  (void)_schemaObjectsCache[{schema, ""}];
  _topLevelCache["schemas"].insert(schema);

  // Refreshing views and tables will implicitly refresh their local objects too.
  addPendingRefresh(RefreshTask::RefreshTables, schema);
  addPendingRefresh(RefreshTask::RefreshViews, schema);
  addPendingRefresh(RefreshTask::RefreshProcedures, schema);
  addPendingRefresh(RefreshTask::RefreshFunctions, schema);

  return true;
}

//--------------------------------------------------------------------------------------------------

void *MySQLObjectNamesCache::refreshThreadFunction(void *data)
{
  MySQLObjectNamesCache *self = reinterpret_cast<MySQLObjectNamesCache*>(data);
  self->refreshThread();

  return nullptr;
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshSchemas()
{
  std::vector<std::pair<std::string, std::string>> result = _getValues("show databases");
  std::set<std::string> schemas;

  for (auto entry : result)
    schemas.insert(entry.first);

  if (!_shutdown)
  {
    updateSchemas(schemas);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "schemas";
    base::NotificationCenter::get()->send("GNObjectCache", this, info);

  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshTables(const std::string &schema)
{
  std::string sql = base::sqlstring("SHOW FULL TABLES FROM !", 0) << schema;
  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);

  // Using a StringListPtr here as this is what the public API uses (to avoid copying data when
  // updating from outside).
  base::StringListPtr tables(new std::list<std::string>());

  for (auto entry : result)
  {
    if (entry.second != "VIEW")
    {
      tables->push_back(entry.first);

      // Implicitly load table-local objects for each table/view.
      addPendingRefresh(RefreshTask::RefreshColumns, schema, entry.first);
      addPendingRefresh(RefreshTask::RefreshTriggers, schema, entry.first);
    }
  }

  if (!_shutdown)
  {
    updateObjectNames("tables", schema, tables);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "tables";
    info["path"] = "\0" + schema;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshViews(const std::string &schema)
{
  std::string sql = base::sqlstring("SHOW FULL TABLES FROM !", 0) << schema;
  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);
  base::StringListPtr views(new std::list<std::string>());

  for (auto entry : result)
  {
    if (entry.second == "VIEW")
    {
      views->push_back(entry.first);
      addPendingRefresh(RefreshTask::RefreshColumns, schema, entry.first);
    }
  }

  if (!_shutdown)
  {
    updateObjectNames("views", schema, views);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "views";
    info["path"] = "\0" + schema;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshFunctions(const std::string &schema)
{
  std::string sql = base::sqlstring("SHOW FUNCTION STATUS WHERE Db=?", 0) << schema;
  base::StringListPtr functions(new std::list<std::string>());
  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);

  for (auto entry : result)
    functions->push_back(entry.second);

  if (!_shutdown)
  {
    updateObjectNames("functions", schema, functions);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "functions";
    info["path"] = "\0" + schema;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshProcedures(const std::string &schema)
{
  std::string sql = base::sqlstring("SHOW PROCEDURE STATUS WHERE Db=?", 0) << schema;
  base::StringListPtr procedures(new std::list<std::string>());

  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);

  for (auto entry : result)
    procedures->push_back(entry.second);

  if (!_shutdown)
  {
    updateObjectNames("procedures", schema, procedures);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "procedures";
    info["path"] = "\0" + schema;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshColumns(const std::string &schema, const std::string &table)
{
  std::string sql = base::sqlstring("SHOW COLUMNS FROM !.!", 0) << schema << table;
  std::set<std::string> columns;
  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);

  for (auto entry : result)
    columns.insert(entry.first);

  if (!_shutdown)
  {
    updateObjectNames(table, schema, columns, ColumnsCacheType);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "columns";
    info["path"] = "\0" + schema + "\0" + table;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshTriggers(const std::string &schema, const std::string &table)
{
  std::string sql;
  if (!table.empty())
    sql = base::sqlstring("SHOW TRIGGERS FROM ! WHERE ! = ?", 0) << schema << "Table" << table;
  else
    sql = base::sqlstring("SHOW TRIGGERS FROM !", 0) << schema;
  std::set<std::string> triggers;
  std::vector<std::pair<std::string, std::string>> result = _getValues(sql);

  for (auto entry : result)
    triggers.insert(entry.first);

  if (!_shutdown)
  {
    updateObjectNames(table, schema, triggers, TriggersCacheType);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "triggers";
    info["path"] = "\0" + schema + "\0" + table;
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshUdfs()
{
  std::set<std::string> udfs;
  std::vector<std::pair<std::string, std::string>> result = _getValues("SELECT NAME FROM mysql.func");

  for (auto entry : result)
    udfs.insert(entry.first);

  if (!_shutdown)
  {
    updateObjectNames("udfs", udfs);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "udfs";
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshVariables()
{
  std::set<std::string> variables;
  std::vector<std::pair<std::string, std::string>> result = _getValues("SHOW GLOBAL VARIABLES");

  for (auto entry : result)
    variables.insert("@@" + entry.first);

  if (!_shutdown)
  {
    updateObjectNames("variables", variables);

    //TODO: this should be called in a way that this notification will be delivered on the main thread only.
    base::NotificationInfo info;
    info["type"] = "variables";
    base::NotificationCenter::get()->send("GNObjectCache", this, info);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshEngines()
{
  std::set<std::string> engines;
  std::vector<std::pair<std::string, std::string>> result = _getValues("SHOW ENGINES");

  for (auto entry : result)
    engines.insert(entry.first);

  if (!_shutdown)
    updateObjectNames("engines", engines);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshLogfileGroups()
{
  std::set<std::string> logfileGroups;

  // Logfile groups and tablespaces are referenced as single unqualified identifiers in MySQL syntax.
  // They are stored however together with a table schema and a table name.
  // For auto completion however we only need to support what the syntax supports.
  std::vector<std::pair<std::string, std::string>> result = _getValues("SELECT logfile_group_name FROM information_schema.FILES");

  for (auto entry : result)
    logfileGroups.insert(entry.first);

  if (!_shutdown)
    updateObjectNames("logfile_groups", logfileGroups);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::doRefreshTablespaces()
{
  std::set<std::string> tablespaces;
  std::vector<std::pair<std::string, std::string>> result = _getValues("SELECT tablespace_name FROM information_schema.FILES");

  for (auto entry : result)
    tablespaces.insert(entry.first);

  if (!_shutdown)
    updateObjectNames("tablespaces", tablespaces);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::updateSchemas(const std::set<std::string> &schemas)
{
  logDebug3("Updating schema list");

  base::RecMutexLock lock(_cacheLock);

  // Remove all schema entries which are not in the given schema list.
  // Collect removal candidates for all 3 caches.
  std::vector<std::string> removalCandidates;
  for (auto schema : _topLevelCache["schemas"])
  {
    if (schemas.find(schema) == schemas.end())
      removalCandidates.push_back(schema);
  }

  for (auto &schema : removalCandidates)
  {
    _topLevelCache["schemas"].erase(schema);
    
    _schemaObjectsCache.erase({schema, "views"});
    _schemaObjectsCache.erase({schema, "tables"});
    _schemaObjectsCache.erase({schema, "functions"});
    _schemaObjectsCache.erase({schema, "procedures"});
    _schemaObjectsCache.erase({schema, "trigger"});
    _schemaObjectsCache.erase({schema, "events"});

    _tableObjectsCache.erase(schema);
  }

  // Next add all schemas to the cache that aren't there yet.
  // Accessing a key that doesn't exist auto creates an entry so just touch all entries from schemas.
  for (auto schema : schemas)
  {
    _topLevelCache["schemas"].insert(schema);
  }
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::updateTables(const std::string &schema, base::StringListPtr tables)
{
  updateObjectNames("tables", schema, tables);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::updateViews(const std::string &schema, base::StringListPtr views)
{
  updateObjectNames("views", schema, views);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::updateProcedures(const std::string &schema, base::StringListPtr procedures)
{
  updateObjectNames("procedures", schema, procedures);
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::updateFunctions(const std::string &schema, base::StringListPtr functions)
{
  updateObjectNames("functions", schema, functions);
}

//--------------------------------------------------------------------------------------------------

/**
 * Update routine for the top level cache.
 */
void MySQLObjectNamesCache::updateObjectNames(const std::string &cache, const std::set<std::string> &objects)
{
  base::RecMutexLock lock(_cacheLock);
  _topLevelCache[cache] = objects;
}

//--------------------------------------------------------------------------------------------------

/**
 * Update routine for schema objects.
 * We use here a (less efficient) list for the objects instead of a set, as we use this code also
 * from code outside of this class.
 */
void MySQLObjectNamesCache::updateObjectNames(const std::string &cache, const std::string &schema,
  base::StringListPtr objects)
{
  std::set<std::string> objectSet;
  for (auto entry : *objects)
    objectSet.insert(entry);
  updateObjectNames(cache, schema, objectSet, OtherCacheType);
}

//--------------------------------------------------------------------------------------------------

// Update routine for either schema objects or columns. The context parameter has a different meaning
// depending on whether we are updating schema objects or columns.
// In the first case it's the object type name, otherwise the table name.
void MySQLObjectNamesCache::updateObjectNames(const std::string &context, const std::string &schema,
  const std::set<std::string> &objects, CacheObjectType type)
{
  base::RecMutexLock lock(_cacheLock);
  if (type == OtherCacheType)
    _schemaObjectsCache[{schema, context}] = objects;
  else
    _tableObjectsCache[schema][context][type] = objects;
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::addPendingRefresh(RefreshTask::RefreshType type, const std::string &schema,
  const std::string &table)
{
  base::RecMutexLock lock(_pendingMutex);
  if (_shutdown)
    return;

  // Add the new task only if there isn't already one of the same type and for the same objects.
  bool found = false;
  for (auto &task : _pendingTasks)
  {
    if (task.type != type)
      continue;

    switch (type) {
      case RefreshTask::RefreshSchemas:
      case RefreshTask::RefreshVariables:
      case RefreshTask::RefreshEngines:
      case RefreshTask::RefreshUDFs:
        found = true;
        break;

      case RefreshTask::RefreshTables:
      case RefreshTask::RefreshViews:
      case RefreshTask::RefreshProcedures:
      case RefreshTask::RefreshFunctions:
        found = task.schemaName == schema;
        break;

      case RefreshTask::RefreshTriggers:
      case RefreshTask::RefreshColumns:
      case RefreshTask::RefreshLogfileGroups:
      case RefreshTask::RefreshTableSpaces:
        found = (task.schemaName == schema) && (task.tableName == table);
        break;
    }
    if (found)
      break;
  }

  if (!found)
    _pendingTasks.push_back(RefreshTask(type, schema, table));

  // Create the worker thread if there's work to do. Does nothing if there's already a thread.
  if (_pendingTasks.size() > 0)
    createWorkerThread();
}

//--------------------------------------------------------------------------------------------------

bool MySQLObjectNamesCache::getPendingRefresh(RefreshTask &task)
{
  bool result = false;

  base::RecMutexLock lock(_pendingMutex);
  if (_shutdown)
    return result;

  if (!_pendingTasks.empty())
  {
    result = true;
    task = _pendingTasks.front();
    _pendingTasks.pop_front();
  }

  return result;
}

//--------------------------------------------------------------------------------------------------

void MySQLObjectNamesCache::createWorkerThread()
{
  // Fire up thread to start caching.
  if (!_cacheWworking.try_wait()) // If there is already worker thread, just do nothing and exit.
    return;

  // We need to wait for previous thread to finish before we create new thread.
  if (_refreshThread != NULL)
  {
    g_thread_join(_refreshThread);
    _refreshThread = NULL;
  }

  if (!_shutdown)
  {
    logDebug3("Creating worker thread\n");

    GError *error = NULL;
    _refreshThread = base::create_thread(&MySQLObjectNamesCache::refreshThreadFunction, this, &error);
    if (!_refreshThread)
    {
      logError("Error creating autocompletion worker thread: %s\n", error ? error->message : "out of mem?");
      g_error_free(error);
    }
    else
      if (_feedback)
        _feedback(true);
  }
}

//--------------------------------------------------------------------------------------------------
