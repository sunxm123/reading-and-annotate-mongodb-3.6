// test_commands.cpp

/**
*    Copyright (C) 2013-2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include <string>

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/initializer_context.h"
#include "mongo/db/catalog/capped_utils.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {

using repl::UnreplicatedWritesBlock;
using std::endl;
using std::string;
using std::stringstream;

/* For testing only, not for general use. Enabled via command-line */
//以下几个命令需要mongod --setParameter=enableTestCommands才能生效
class GodInsert : public ErrmsgCommandDeprecated {
public:
    GodInsert() : ErrmsgCommandDeprecated("godinsert") {}
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool slaveOk() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}
    virtual void help(stringstream& help) const {
        help << "internal. for testing only.";
    }
    virtual bool errmsgRun(OperationContext* opCtx,
                           const string& dbname,
                           const BSONObj& cmdObj,
                           string& errmsg,
                           BSONObjBuilder& result) {
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));
        log() << "test only command godinsert invoked coll:" << nss.coll();
        BSONObj obj = cmdObj["obj"].embeddedObjectUserCheck();

        Lock::DBLock lk(opCtx, dbname, MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        Database* db = ctx.db();

        WriteUnitOfWork wunit(opCtx);
        UnreplicatedWritesBlock unreplicatedWritesBlock(opCtx);
        Collection* collection = db->getCollection(opCtx, nss);
        if (!collection) {
            collection = db->createCollection(opCtx, nss.ns());
            if (!collection) {
                errmsg = "could not create collection";
                return false;
            }
        }
        OpDebug* const nullOpDebug = nullptr;
        Status status = collection->insertDocument(opCtx, InsertStatement(obj), nullOpDebug, false);
        if (status.isOK()) {
            wunit.commit();
        }
        return appendCommandStatus(result, status);
    }
};

/* for diagnostic / testing purposes. Enabled via command line. */
//以下几个命令需要mongod --setParameter=enableTestCommands才能生效
//db.runCommand( { sleep: 1 } ); sleep 10s
class CmdSleep : public BasicCommand {
public:
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool slaveOk() const {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "internal testing command. Run a no-op command for an arbitrary amount of time. ";
        help << "If neither 'secs' nor 'millis' is set, command will sleep for 10 seconds. ";
        help << "If both are set, command will sleep for the sum of 'secs' and 'millis.'\n";
        help << "   w:<bool> (deprecated: use 'lock' instead) if true, takes a write lock.\n";
        help << "   lock: r, w, none. If r or w, db will block under a lock. Defaults to r.";
        help << " 'lock' and 'w' may not both be set.\n";
        help << "   secs:<seconds> Amount of time to sleep, in seconds.\n";
        help << "   millis:<milliseconds> Amount of time to sleep, in ms.\n";
    }

    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}

    void _sleepInReadLock(mongo::OperationContext* opCtx, long long millis) {
        Lock::GlobalRead lk(opCtx);
        opCtx->sleepFor(Milliseconds(millis));
    }

    void _sleepInWriteLock(mongo::OperationContext* opCtx, long long millis) {
        Lock::GlobalWrite lk(opCtx);
        opCtx->sleepFor(Milliseconds(millis));
    }

    CmdSleep() : BasicCommand("sleep") {}
    bool run(OperationContext* opCtx,
             const string& ns,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        log() << "test only command sleep invoked";
        long long millis = 0;

        if (cmdObj["secs"] || cmdObj["millis"]) {
            if (cmdObj["secs"]) {
                uassert(34344, "'secs' must be a number.", cmdObj["secs"].isNumber());
                millis += cmdObj["secs"].numberLong() * 1000;
            }
            if (cmdObj["millis"]) {
                uassert(34345, "'millis' must be a number.", cmdObj["millis"].isNumber());
                millis += cmdObj["millis"].numberLong();
            }
        } else {
            millis = 10 * 1000;
        }

        if (!cmdObj["lock"]) {
            // Legacy implementation
            if (cmdObj.getBoolField("w")) {
                _sleepInWriteLock(opCtx, millis);
            } else {
                _sleepInReadLock(opCtx, millis);
            }
        } else {
            uassert(34346, "Only one of 'w' and 'lock' may be set.", !cmdObj["w"]);

            std::string lock(cmdObj.getStringField("lock"));
            if (lock == "none") {
                opCtx->sleepFor(Milliseconds(millis));
            } else if (lock == "w") {
                _sleepInWriteLock(opCtx, millis);
            } else {
                uassert(34347, "'lock' must be one of 'r', 'w', 'none'.", lock == "r");
                _sleepInReadLock(opCtx, millis);
            }
        }

        // Interrupt point for testing (e.g. maxTimeMS).
        opCtx->checkForInterrupt();

        return true;
    }
};

// Testing only, enabled via command-line.
//以下几个命令需要mongod --setParameter=enableTestCommands=1才能生效

class CapTrunc : public BasicCommand {
public:
    CapTrunc() : BasicCommand("captrunc") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}
    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString fullNs = parseNsCollectionRequired(dbname, cmdObj);
        if (!fullNs.isValid()) {
            return appendCommandStatus(
                result,
                {ErrorCodes::InvalidNamespace,
                 str::stream() << "collection name " << fullNs.ns() << " is not valid"});
        }

        int n = cmdObj.getIntField("n");
        bool inc = cmdObj.getBoolField("inc");  // inclusive range?

        if (n <= 0) {
            return appendCommandStatus(result,
                                       {ErrorCodes::BadValue, "n must be a positive integer"});
        }

        // Lock the database in mode IX and lock the collection exclusively.
        AutoGetCollection autoColl(opCtx, fullNs, MODE_IX, MODE_X);
        Collection* collection = autoColl.getCollection();
        if (!collection) {
            return appendCommandStatus(
                result,
                {ErrorCodes::NamespaceNotFound,
                 str::stream() << "collection " << fullNs.ns() << " does not exist"});
        }

        if (!collection->isCapped()) {
            return appendCommandStatus(result,
                                       {ErrorCodes::IllegalOperation, "collection must be capped"});
        }

        RecordId end;
        {
            // Scan backwards through the collection to find the document to start truncating from.
            // We will remove 'n' documents, so start truncating from the (n + 1)th document to the
            // end.
            auto exec = InternalPlanner::collectionScan(
                opCtx, fullNs.ns(), collection, PlanExecutor::NO_YIELD, InternalPlanner::BACKWARD);

            for (int i = 0; i < n + 1; ++i) {
                PlanExecutor::ExecState state = exec->getNext(nullptr, &end);
                if (PlanExecutor::ADVANCED != state) {
                    return appendCommandStatus(
                        result,
                        {ErrorCodes::IllegalOperation,
                         str::stream() << "invalid n, collection contains fewer than " << n
                                       << " documents"});
                }
            }
        }

        collection->cappedTruncateAfter(opCtx, end, inc);

        return true;
    }
};

// Testing-only, enabled via command line.
//以下几个命令需要mongod --setParameter=enableTestCommands才能生效

class EmptyCapped : public BasicCommand {
public:
    EmptyCapped() : BasicCommand("emptycapped") {}
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }
    // No auth needed because it only works when enabled via command line.
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {}

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        const NamespaceString nss = parseNsCollectionRequired(dbname, cmdObj);

        return appendCommandStatus(result, emptyCapped(opCtx, nss));
    }
};

// ----------------------------

//以下几个命令需要mongod --setParameter=enableTestCommands才能生效
MONGO_INITIALIZER(RegisterEmptyCappedCmd)(InitializerContext* context) {
    if (Command::testCommandsEnabled) {
        // Leaked intentionally: a Command registers itself when constructed.
        new CapTrunc();
        new CmdSleep();
        new EmptyCapped();
        new GodInsert();
    }
    return Status::OK();
}
}
