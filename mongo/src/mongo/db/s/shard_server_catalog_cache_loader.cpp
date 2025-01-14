/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_catalog_cache_loader.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using namespace shardmetadatautil;

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

AtomicUInt64 taskIdGenerator{0};

/**
 * Constructs the options for the loader thread pool.
 */ //每个线程名都有一个num，参考ShardServerCatalogCacheLoader::Task::Task
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ShardServerCatalogCacheLoader";
    options.minThreads = 0;
    options.maxThreads = 6;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

/**
 * Takes a CollectionAndChangedChunks object and persists the changes to the shard's metadata
 * collections.
 *
 * Returns ConflictingOperationInProgress if a chunk is found with a new epoch.
 */ 

//ShardServerCatalogCacheLoader::_updatePersistedMetadata
//把拿到的变化的collAndChunks信息更新到cache.chunks.库.表中
Status persistCollectionAndChangedChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionAndChangedChunks& collAndChunks) {
    // Update the collections collection entry for 'nss' in case there are any new updates.
    //config.cache.collections表中的nss表内容，代表一个分片集合信息
    ShardCollectionType update = ShardCollectionType(nss,
                                                     collAndChunks.uuid,
                                                     collAndChunks.epoch,
                                                     collAndChunks.shardKeyPattern,
                                                     collAndChunks.defaultCollation,
                                                     collAndChunks.shardKeyIsUnique);
	//把collAndChunks信息更新到"config.cache.collections"，没有则写入
	//更新"config.cache.collections"表中的内容，更新分片表信息
	Status status = updateShardCollectionsEntry(opCtx,
                                                BSON(ShardCollectionType::ns() << nss.ns()),
                                                update.toBSON(),
                                                BSONObj(),
                                                true /*upsert*/);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as refreshing, so that secondaries are aware of refresh.
    //设置cache.collections表中的对应表的refreshing字段为true，标记当前真再刷新chunk路由信息
    status = setPersistedRefreshFlags(opCtx, nss);
    if (!status.isOK()) {
        return status;
    }

    // Update the chunks.
    //更新config.cache.chunks.表 中的chunks信息到最新的chunks，先找出有表中和chunks有交集的chunk，然后删除插入新的chunks
    status = updateShardChunks(opCtx, nss, collAndChunks.changedChunks, collAndChunks.epoch);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as done refreshing.
    //设置cache.collections表中的对应表的refreshing字段为false，标记当前刷新chunk路由信息结束
    status =
        unsetPersistedRefreshFlags(opCtx, nss, collAndChunks.changedChunks.back().getVersion());
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

/**
 * This function will throw on error!
 *
 * Retrieves the persisted max chunk version for 'nss', if there are any persisted chunks. If there
 * are none -- meaning there's no persisted metadata for 'nss' --, returns a
 * ChunkVersion::UNSHARDED() version.
 *
 * It is unsafe to call this when a task for 'nss' is running concurrently because the collection
 * could be dropped and recreated between reading the collection epoch and retrieving the chunk,
 * which would make the returned ChunkVersion corrupt.
 */ //ShardServerCatalogCacheLoader::_schedulePrimaryGetChunksSince
ChunkVersion getPersistedMaxVersion(OperationContext* opCtx, const NamespaceString& nss) {
    // Must read the collections entry to get the epoch to pass into ChunkType for shard's chunk
    // collection.
    //查找config.cache.collections表中的nss表内容，只有该表启用了分片功能，config.cache.collections中才会有该表的一条数据
    auto statusWithCollection = readShardCollectionsEntry(opCtx, nss);
    if (statusWithCollection == ErrorCodes::NamespaceNotFound) {
        // There is no persisted metadata.
        return ChunkVersion::UNSHARDED();
    }
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read persisted collections entry for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithCollection.getStatus().toString()
                          << "'.",
            statusWithCollection.isOK());

	//按照参数中指定条件读取"config.cache.chunks."中内容的最新一条数据，倒排序
	//db.cache.chunks.db.collection.find().sort({lastmod:-1}).limit(1)
	//读取db.cache.chunks.db.collection表中最大的一个chunk，也就是lastmod最大的chunk
    auto statusWithChunk =
        shardmetadatautil::readShardChunks(opCtx,
                                           nss,
                                           BSONObj(),
                                           BSON(ChunkType::lastmod() << -1),
                                           1LL,
                                           statusWithCollection.getValue().getEpoch());
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read highest version persisted chunk for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithChunk.getStatus().toString()
                          << "'.",
            statusWithChunk.isOK());

    return statusWithChunk.getValue().empty() ? ChunkVersion::UNSHARDED()
                                              : statusWithChunk.getValue().front().getVersion();
}

/**
 * This function will throw on error!
 *
 * Tries to find persisted chunk metadata with chunk versions GTE to 'version'.
 *
 * If 'version's epoch matches persisted metadata, returns persisted metadata GTE 'version'.
 * If 'version's epoch doesn't match persisted metadata, returns all persisted metadata.
 * If collections entry does not exist, throws NamespaceNotFound error. Can return an empty
 * chunks vector in CollectionAndChangedChunks without erroring, if collections entry IS found.
 */ 
//如果version.epoll和config.cache.collections表epoll不一致则获取全量chunk数据，否则获取增量chunk数据
CollectionAndChangedChunks getPersistedMetadataSinceVersion(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            ChunkVersion version,
                                                            const bool okToReadWhileRefreshing) {
    ShardCollectionType shardCollectionEntry =
		//查找config.cache.collections表中的nss表内容，启用了分片功能的表这里面都会有记录
        uassertStatusOK(readShardCollectionsEntry(opCtx, nss));

    // If the persisted epoch doesn't match what the CatalogCache requested, read everything.
    //config.cache.collections表中对应表的epoch和version请求中的epoll是否一致
    ChunkVersion startingVersion = (shardCollectionEntry.getEpoch() == version.epoch())
        ? version
        : ChunkVersion(0, 0, shardCollectionEntry.getEpoch());

	//类似db.cache.chunks.db.coll.find({"lastmod" :{$gte:Timestamp(xx, xx)}}).sort({"lastmod" : 1})
    QueryAndSort diff = createShardChunkDiffQuery(startingVersion);

	//获取({"lastmod" :{$gte:Timestamp(xx, xx)}}).sort({"lastmod" : 1})条件的增量数据，也就是获取增量chunk信息
    auto changedChunks = uassertStatusOK(
        readShardChunks(opCtx, nss, diff.query, diff.sort, boost::none, startingVersion.epoch()));

    return CollectionAndChangedChunks{shardCollectionEntry.getUUID(),
                                      shardCollectionEntry.getEpoch(),
                                      shardCollectionEntry.getKeyPattern().toBSON(),
                                      shardCollectionEntry.getDefaultCollation(),
                                      shardCollectionEntry.getUnique(),
                                      std::move(changedChunks)};
}

/**
 * Attempts to read the collection and chunk metadata. May not read a complete diff if the metadata
 * for the collection is being updated concurrently. This is safe if those updates are appended.
 *
 * If the epoch changes while reading the chunks, returns an empty object.
 */
//获取db.cache.chunks.db.coll表chunks过程中如果变化的chunks过多，则需要时间。获取chunks过程中，有可能表被删除了，因此还需要检查一次epoch
StatusWith<CollectionAndChangedChunks> getIncompletePersistedMetadataSinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, ChunkVersion version) {

    try {
		//如果version.epoll和config.cache.collections表epoll不一致则获取全量chunk数据，否则获取增量chunk数据
        CollectionAndChangedChunks collAndChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version, false);
        if (collAndChunks.changedChunks.empty()) {
            // Found a collections entry, but the chunks are being updated.
            return CollectionAndChangedChunks();
        }

        // Make sure the collections entry epoch has not changed since we began reading chunks --
        // an epoch change between reading the collections entry and reading the chunk metadata
        // would invalidate the chunks.

		//查找config.cache.collections表中的nss表内容，启用了分片功能的表这里面都会有记录
        auto afterShardCollectionsEntry = uassertStatusOK(readShardCollectionsEntry(opCtx, nss));
        if (collAndChunks.epoch != afterShardCollectionsEntry.getEpoch()) {
            // The collection was dropped and recreated since we began. Return empty results.
            return CollectionAndChangedChunks();
        }

        return collAndChunks;
    } catch (const DBException& ex) {
        Status status = ex.toStatus();
        if (status == ErrorCodes::NamespaceNotFound) {
            return CollectionAndChangedChunks();
        }
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to load local metadata due to '" << status.toString()
                                    << "'.");
    }
}

/**
 * Sends _flushRoutingTableCacheUpdates to the primary to force it to refresh its routing table for
 * collection 'nss' and then waits for the refresh to replicate to this node.
 */

/*
以下情况下，该命令不会返回从节点会卡住: 4.0.3版本
Fri Mar 18 13:14:46.868 I SHARDING [ShardServerCatalogCacheLoader-1927] Failed to persist chunk metadata update for collection 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_DB.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb_COLLECTION :: caused by :: InvalidNamespace: Failed to update the persisted chunk metadata for collection 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_DB.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb_COLLECTION' from '0|0||000000000000000000000000' to '1|0||62333e4eb3e60f88b9e94ecc'. Will be retried. :: caused by :: fully qualified namespace config.cache.chunks.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_DB.bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb_COLLECTION is too long (max is 120 bytes)

*/


//forcePrimaryRefreshAndWaitForReplication：
//从节点通过_flushRoutingTableCacheUpdates发送给主节点，主节点开始获取最新的路由信息


//ShardServerCatalogCacheLoader::getChunksSince->ShardServerCatalogCacheLoader::_runSecondaryGetChunksSince调用
//主节点收到该命令后执行的地方见FlushRoutingTableCacheUpdates
void forcePrimaryRefreshAndWaitForReplication(OperationContext* opCtx, const NamespaceString& nss) {
    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->enabled());
	
    auto selfShard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardingState->getShardName()));

	//通过主节点执行FlushRoutingTableCacheUpdates()获取到真正的返回结果
	//FlushRoutingTableCacheUpdates一定要等迁移关键阶段结束才返回，注意这里可能等待很长一段时间，但是这里限制了30秒，所以还好，最多30秒返回
    auto cmdResponse = uassertStatusOK(selfShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        //"_flushRoutingTableCacheUpdates"和"forceRoutingTableRefresh"等价，参考FlushRoutingTableCacheUpdates()
        BSON("forceRoutingTableRefresh" << nss.ns()),
        Seconds{30}, //等待超时时间30秒
        //如果是因为WriteConcernFailed相关的错误，则可以重试执行SQL，否则不重试
        Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(cmdResponse.commandStatus);

	//一直等主节点的op time到来才会返回，这里可能会阻塞，如果有主从延迟
    uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(
        opCtx, {LogicalTime::fromOperationTime(cmdResponse.response), boost::none}));
}

/**
 * Reads the local chunk metadata to obtain the current ChunkVersion. If there is no local
 * metadata for the namespace, returns ChunkVersion::UNSHARDED(), since only metadata for sharded
 * collections is persisted.
 */
//获取nss表的版本信息，从config.cache.collections中读取"lastRefreshedCollectionVersion" : Timestamp(13, 4)
ChunkVersion getLocalVersion(OperationContext* opCtx, const NamespaceString& nss) {
	//config.cache.collections表中的nss表内容refreshing字段，表示当前是否真在刷新全量路由或者增量路由
    auto swRefreshState = getPersistedRefreshFlags(opCtx, nss);
    if (swRefreshState == ErrorCodes::NamespaceNotFound)
        return ChunkVersion::UNSHARDED();
    return uassertStatusOK(std::move(swRefreshState)).lastRefreshedCollectionVersion;
}

}  // namespace


ShardServerCatalogCacheLoader::ShardServerCatalogCacheLoader(
    std::unique_ptr<CatalogCacheLoader> configServerLoader)
    //ShardServerCatalogCacheLoader._configServerLoader指向本分片对应的config server
    : _configServerLoader(std::move(configServerLoader)),
      _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ShardServerCatalogCacheLoader::~ShardServerCatalogCacheLoader() {
    // Prevent further scheduling, then interrupt ongoing tasks.
    _threadPool.shutdown();
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _contexts.interrupt(ErrorCodes::InterruptedAtShutdown);
        ++_term;
    }

    _threadPool.join();
    invariant(_contexts.isEmpty());
}

//配合_getCompletePersistedMetadataForSecondarySinceVersion阅读
//CollectionVersionLogOpHandler调用
void ShardServerCatalogCacheLoader::notifyOfCollectionVersionUpdate(const NamespaceString& nss) {
    _namespaceNotifications.notifyChange(nss);
}

//ShardingState::initializeFromShardIdentity中调用
void ShardServerCatalogCacheLoader::initializeReplicaSetRole(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role == ReplicaSetRole::None);

    if (isPrimary) {
        _role = ReplicaSetRole::Primary;
    } else {
        _role = ReplicaSetRole::Secondary;
    }
}

//主从状态发生变化，则_term自增
void ShardServerCatalogCacheLoader::onStepDown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.interrupt(ErrorCodes::PrimarySteppedDown);
    ++_term;
    _role = ReplicaSetRole::Secondary;
}

//_shardingOnTransitionToPrimaryHook
void ShardServerCatalogCacheLoader::onStepUp() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    ++_term;
    _role = ReplicaSetRole::Primary;
}

//CatalogCache::_scheduleCollectionRefresh调用 
std::shared_ptr<Notification<void>> 
  ShardServerCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss,
    ChunkVersion version,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
    long long currentTerm;
    bool isPrimary;
    {
        // Take the mutex so that we can discern whether we're primary or secondary and schedule a
        // task with the corresponding _term value.
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_role != ReplicaSetRole::None);

		//记录当前节点状态，刷新路由过程中主从切换通过该值判断
        currentTerm = _term;
        isPrimary = (_role == ReplicaSetRole::Primary);
    }

    auto notify = std::make_shared<Notification<void>>();

    uassertStatusOK(_threadPool.schedule(
        [ this, nss, version, callbackFn, notify, isPrimary, currentTerm ]() noexcept {
            auto context = _contexts.makeOperationContext(*Client::getCurrent());

            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                // We may have missed an OperationContextGroup interrupt since this operation began
                // but before the OperationContext was added to the group. So we'll check that
                // we're still in the same _term.
                //主从状态发生变化或者节点处于shutdown状态
                if (_term != currentTerm) {
                    callbackFn(context.opCtx(),
                               Status{ErrorCodes::Interrupted,
                                      "Unable to refresh routing table because replica set state "
                                      "changed or node is shutting down."});
                    notify->set();
                    return;
                }
            }

            try {
				//主节点走该分支
                if (isPrimary) {
                    _schedulePrimaryGetChunksSince(
                        context.opCtx(), nss, version, currentTerm, callbackFn, notify);
                } else { //从节点走该分支
                    _runSecondaryGetChunksSince(context.opCtx(), nss, version, callbackFn);
                }
            } catch (const DBException& ex) {
                callbackFn(context.opCtx(), ex.toStatus());
                notify->set();
            }
        }));

    return notify;
}

////FlushRoutingTableCacheUpdates::run  commitChunkMetadataOnConfig
void ShardServerCatalogCacheLoader::waitForCollectionFlush(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lg(_mutex);
    const auto initialTerm = _term;

    boost::optional<uint64_t> taskNumToWait;

    while (true) {
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Unable to wait for collection metadata flush for " << nss.ns()
                              << " because the node's replication role changed.",
                _role == ReplicaSetRole::Primary && _term == initialTerm);

        auto it = _taskLists.find(nss);

        // If there are no tasks for the specified namespace, everything must have been completed
        if (it == _taskLists.end())
            return;

        auto& taskList = it->second;

        if (!taskNumToWait) {
            const auto& lastTask = taskList.back();
            taskNumToWait = lastTask.taskNum;
        } else {
            const auto& activeTask = taskList.front();

            if (activeTask.taskNum > *taskNumToWait) {
                auto secondTaskIt = std::next(taskList.begin());

                // Because of an optimization where a namespace drop clears all tasks except the
                // active it is possible that the task number we are waiting on will never actually
                // be written. Because of this we move the task number to the drop which can only be
                // in the active task or in the one after the active.
                if (activeTask.dropped) {
                    taskNumToWait = activeTask.taskNum;
                } else if (secondTaskIt != taskList.end() && secondTaskIt->dropped) {
                    taskNumToWait = secondTaskIt->taskNum;
                } else {
                    return;
                }
            }
        }

        // It is not safe to use taskList after this call, because it will unlock and lock the tasks
        // mutex, so we just loop around.
        //
        taskList.waitForActiveTaskCompletion(lg);
    }
}

//primary调用_schedulePrimaryGetChunksSince，secondary调用_runSecondaryGetChunksSince
//ShardServerCatalogCacheLoader::getChunksSince调用
void ShardServerCatalogCacheLoader::_runSecondaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
	//从节点通过_flushRoutingTableCacheUpdates发送给主节点，主节点开始获取最新的路由信息
	forcePrimaryRefreshAndWaitForReplication(opCtx, nss);

    // Read the local metadata.
    auto swCollAndChunks =
        _getCompletePersistedMetadataForSecondarySinceVersion(opCtx, nss, catalogCacheSinceVersion);
    callbackFn(opCtx, std::move(swCollAndChunks));
}

//primary调用_schedulePrimaryGetChunksSince，secondary调用_runSecondaryGetChunksSince
//ShardServerCatalogCacheLoader::getChunksSince
void ShardServerCatalogCacheLoader::_schedulePrimaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    long long termScheduled,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn,
    std::shared_ptr<Notification<void>> notify) {

    // Get the max version the loader has.
    const ChunkVersion maxLoaderVersion = [&] {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            auto taskListIt = _taskLists.find(nss);

			//队列queue中的也就是内存中的，也就是从config server获取到的chunk，还没有写入config.cache.chunks.db.collection中的
            if (taskListIt != _taskLists.end() &&
                taskListIt->second.hasTasksFromThisTerm(termScheduled)) {
                // Enqueued tasks have the latest metadata
                return taskListIt->second.getHighestVersionEnqueued();
            }
        }

        // If there are no enqueued tasks, get the max persisted
	    //按照参数中指定条件读取"config.cache.chunks."中内容的最新一条数据，倒排序
		//db.cache.chunks.db.collection.find().sort({lastmod:-1}).limit(1)
		//读取db.cache.chunks.db.collection表中最大的一个chunk，也就是lastmod最大的chunk
        return getPersistedMaxVersion(opCtx, nss);
    }();

    auto remoteRefreshCallbackFn = [this,
                                    nss,
                                    catalogCacheSinceVersion,
                                    maxLoaderVersion,
                                    termScheduled,
                                    callbackFn,
                                    notify](
        OperationContext* opCtx,
        //来源见ConfigServerCatalogCacheLoader::getChunksSince
        StatusWith<CollectionAndChangedChunks> swCollectionAndChangedChunks) {

        if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
            Status scheduleStatus = _ensureMajorityPrimaryAndScheduleTask(
                opCtx, nss, Task{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
            if (!scheduleStatus.isOK()) {
                callbackFn(opCtx, scheduleStatus);
                notify->set();
                return;
            }
			
            log() << "Cache loader remotely refreshed for collection " << nss << " from version "
                  << maxLoaderVersion << " and no metadata was found.";
        } else if (swCollectionAndChangedChunks.isOK()) {
            auto& collAndChunks = swCollectionAndChangedChunks.getValue();
			//epoch检查
            if (collAndChunks.changedChunks.back().getVersion().epoch() != collAndChunks.epoch) {
                swCollectionAndChangedChunks =
                    Status{ErrorCodes::ConflictingOperationInProgress,
                           str::stream()
                               << "Invalid chunks found when reloading '"
                               << nss.toString()
                               << "' Previous collection epoch was '"
                               << collAndChunks.epoch.toString()
                               << "', but found a new epoch '"
                               << collAndChunks.changedChunks.back().getVersion().epoch().toString()
                               << "'. Collection was dropped and recreated."};
            } else {
            	//可以通过 db.adminCommand({_flushRoutingTableCacheUpdates: "test.MD_FCT_IER_DETAIL", syncFromConfig: true});强制走到该流程
                if ((collAndChunks.epoch != maxLoaderVersion.epoch()) ||
                    (collAndChunks.changedChunks.back().getVersion() > maxLoaderVersion)) {
                    //写一个noop到多数节点成功，然后将task任务添加到线程池任务队列中，task任务实际上就是_runTasks中把拿到的
                    //变化的chunks写到本地config cache chunks表中
                    log() << "yang test 111.  _schedulePrimaryGetChunksSince";
                    Status scheduleStatus = _ensureMajorityPrimaryAndScheduleTask(
                        opCtx,
                        nss,
                        Task{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
                    if (!scheduleStatus.isOK()) {
                        callbackFn(opCtx, scheduleStatus);
                        notify->set();
                        return;
                    }
					log() << "yang test 222.  _schedulePrimaryGetChunksSince";
                }

				//到这里的时间消耗是从config获取变化的chunk信息到本地的时间
                log() << "Cache loader remotely refreshed for collection " << nss
                      << " from collection version " << maxLoaderVersion
                      << " and found collection version "
                      << collAndChunks.changedChunks.back().getVersion();

                // Metadata was found remotely -- otherwise would have received NamespaceNotFound
                // rather than Status::OK(). Return metadata for CatalogCache that's GTE
                // catalogCacheSinceVersion, from the loader's persisted and enqueued metadata.

                swCollectionAndChangedChunks =
                    _getLoaderMetadata(opCtx, nss, catalogCacheSinceVersion, termScheduled);
                if (swCollectionAndChangedChunks.isOK()) {
                    // After finding metadata remotely, we must have found metadata locally.
                    invariant(!collAndChunks.changedChunks.empty());
                }
            }
        }

        // Complete the callbackFn work.
        callbackFn(opCtx, std::move(swCollectionAndChangedChunks));
        notify->set();
    };

    // Refresh the loader's metadata from the config server. The caller's request will
    // then be serviced from the loader's up-to-date metadata.
    //ConfigServerCatalogCacheLoader::getChunksSince从config获取路由信息
    //拉取cfg中config.chunks表对应版本大于本地缓存lastmod的所有增量变化的chunk
    _configServerLoader->getChunksSince(nss, maxLoaderVersion, remoteRefreshCallbackFn);
}


/**
 * Loads chunk metadata from the shard persisted metadata store and any in-memory tasks with
 * terms matching 'term' enqueued to update that store, GTE to 'catalogCacheSinceVersion'.
 *
 * Will return an empty CollectionAndChangedChunks object if no metadata is found (collection
 * was dropped).
 *
 * Only run on the shard primary.
 */
//ShardServerCatalogCacheLoader::_schedulePrimaryGetChunksSince
StatusWith<CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getLoaderMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {

    // Get the enqueued metadata first. Otherwise we could miss data between reading persisted and
    // enqueued, if an enqueued task finished after the persisted read but before the enqueued read.
	//获取_taskLists中缓存的nss表路由变化信息
    auto enqueuedRes = _getEnqueuedMetadata(nss, catalogCacheSinceVersion, term);
    bool tasksAreEnqueued = std::move(enqueuedRes.first);
    CollectionAndChangedChunks enqueued = std::move(enqueuedRes.second);

	//获取db.cache.chunks.db.coll表中大于catalogCacheSinceVersion的增量chunk信息
    auto swPersisted =
        getIncompletePersistedMetadataSinceVersion(opCtx, nss, catalogCacheSinceVersion);
    CollectionAndChangedChunks persisted;
    if (swPersisted == ErrorCodes::NamespaceNotFound) {
        // No persisted metadata found, create an empty object.
        persisted = CollectionAndChangedChunks();
    } else if (!swPersisted.isOK()) {
        return swPersisted;
    } else {
        persisted = std::move(swPersisted.getValue());
    }

	/*
Fri Feb 11 23:48:01.975 I SHARDING [ConfigServerCatalogCacheLoader-8547] Cache loader found enqueued metadata from 10|48088||61a355de8444860129c52a42 to 10|48156||61a355de8444860129c52a42 and persisted metadata from 10|47865||61a355de8444860129c52a42 to 10|48088||61a355de8444860129c52a42, GTE cache version 10|47865||61a355de8444860129c52a42
Fri Feb 11 23:48:01.984 I SHARDING [ConfigServerCatalogCacheLoader-8545] Cache loader found enqueued metadata from 42277|53718||61a355b18444860129c524ec to 42277|53782||61a355b18444860129c524ec and persisted metadata from 42277|52080||61a355b18444860129c524ec to 42277|53724||61a355b18444860129c524ec, GTE cache version 42277|52080||61a355b18444860129c524ec
Fri Feb 11 23:48:01.986 I SHARDING [ConfigServerCatalogCacheLoader-8546] Cache loader found enqueued metadata from 10|55674||61a355878444860129c5201a to 10|55683||61a355878444860129c5201a and persisted metadata from 10|53135||61a355878444860129c5201a to 10|55683||61a355878444860129c5201a, GTE cache version 10|53135||61a355878444860129c5201a
	*/
    log() << "Cache loader found "
          << (enqueued.changedChunks.empty()
                  ? (tasksAreEnqueued ? "a drop enqueued" : "no enqueued metadata")
                  : ("enqueued metadata from " +
                     enqueued.changedChunks.front().getVersion().toString() + " to " +
                     enqueued.changedChunks.back().getVersion().toString()))
          << " and " << (persisted.changedChunks.empty()
                             ? "no persisted metadata"
                             : ("persisted metadata from " +
                                persisted.changedChunks.front().getVersion().toString() + " to " +
                                persisted.changedChunks.back().getVersion().toString()))
          << ", GTE cache version " << catalogCacheSinceVersion;

    if (!tasksAreEnqueued) { //内存中没有  例如表对应task已经持久化到cache.chunks表中
        // There are no tasks in the queue. Return the persisted metadata.
        return persisted;
    } else if (persisted.changedChunks.empty() || enqueued.changedChunks.empty() ||
               enqueued.epoch != persisted.epoch) { //内存有 或者 持久化没有  或者 epoch不一致，已内存为准
        // There is a task queue and:
        // - nothing is persisted.
        // - nothing was returned from enqueued, which means the last task enqueued is a drop task.
        // - the epoch changed in the enqueued metadata, which means there's a drop operation
        //   enqueued somewhere.
        // Whichever the cause, the persisted metadata is out-dated/non-existent. Return enqueued
        // results.
        return enqueued;
    } else {
    	//内存和config.cache.chunks.xx中都有，内存里面是直接从config server获取的，是最新的增量数据，他们之间会有重复的
    	//这时候以内存增量数据为准
        // There can be overlap between persisted and enqueued metadata because enqueued work can
        // be applied while persisted was read. We must remove this overlap.

        const ChunkVersion minEnqueuedVersion = enqueued.changedChunks.front().getVersion();

        // Remove chunks from 'persisted' that are GTE the minimum in 'enqueued' -- this is
        // the overlap.
        auto persistedChangedChunksIt = persisted.changedChunks.begin();
        while (persistedChangedChunksIt != persisted.changedChunks.end() &&
               persistedChangedChunksIt->getVersion() < minEnqueuedVersion) {
            ++persistedChangedChunksIt;
        }
        persisted.changedChunks.erase(persistedChangedChunksIt, persisted.changedChunks.end());

        // Append 'enqueued's chunks to 'persisted', which no longer overlaps.
        persisted.changedChunks.insert(persisted.changedChunks.end(),
                                       enqueued.changedChunks.begin(),
                                       enqueued.changedChunks.end());

        return persisted;
    }
}

/**
 * Loads chunk metadata from all in-memory tasks enqueued to update the shard persisted metadata
 * store for collection 'nss' that is GTE 'catalogCacheSinceVersion'. If
 * 'catalogCacheSinceVersion's epoch does not match that of the metadata enqueued, returns all
 * metadata. Ignores tasks with terms that do not match 'term': these are no longer valid.
 *
 * The bool returned in the pair indicates whether there are any tasks enqueued. If none are, it
 * is false. If it is true, and the CollectionAndChangedChunks returned is empty, this indicates
 * a drop was enqueued and there is no metadata.
 *
 * Only run on the shard primary.
 */
//ShardServerCatalogCacheLoader::_getLoaderMetadata
//获取_taskLists中缓存的nss路由变化信息
std::pair<bool, CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getEnqueuedMetadata(
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto taskListIt = _taskLists.find(nss);

	//tasklist没有也就是内存中没有，则直接返回
    if (taskListIt == _taskLists.end()) {
        return std::make_pair(false, CollectionAndChangedChunks());
    } else if (!taskListIt->second.hasTasksFromThisTerm(term)) {
        // If task list does not have a term that matches, there's no valid task data to collect.
        return std::make_pair(false, CollectionAndChangedChunks());
    }

    // Only return task data of tasks scheduled in the same term as the given 'term': older term
    // task data is no longer valid.
    //把task中表的changed chuns信息添加到collAndChunks
    CollectionAndChangedChunks collAndChunks = taskListIt->second.getEnqueuedMetadataForTerm(term);

    // Return all the results if 'catalogCacheSinceVersion's epoch does not match. Otherwise, trim
    // the results to be GTE to 'catalogCacheSinceVersion'.

	//epoch发生变化，则直接返回全量collAndChunks
    if (collAndChunks.epoch != catalogCacheSinceVersion.epoch()) {
        return std::make_pair(true, collAndChunks);
    }

	//epoch一致，则返回version大于catalogCacheSinceVersion的chunk
    auto changedChunksIt = collAndChunks.changedChunks.begin();
    while (changedChunksIt != collAndChunks.changedChunks.end() &&
           changedChunksIt->getVersion() < catalogCacheSinceVersion) {
        ++changedChunksIt;
    }

	//把version低的chunk去除，如果这里面剔除的chunk很多，会不会非常耗时?
    collAndChunks.changedChunks.erase(collAndChunks.changedChunks.begin(), changedChunksIt);

    return std::make_pair(true, collAndChunks);
}

//_schedulePrimaryGetChunksSince
//写一个noop到多数节点成功，然后将task任务添加到线程池中
Status ShardServerCatalogCacheLoader::_ensureMajorityPrimaryAndScheduleTask(
    OperationContext* opCtx, const NamespaceString& nss, Task task) {
    //写一个noop到多数派节点成功才返回，如果这时候主从延迟过高，则这里会卡顿
    log() << "yang test .... 1111 _ensureMajorityPrimaryAndScheduleTask";
    Status linearizableReadStatus = waitForLinearizableReadConcern(opCtx);
    if (!linearizableReadStatus.isOK()) {
        return {linearizableReadStatus.code(),
                str::stream() << "Unable to schedule routing table update because this is not the"
                              << " majority primary and may not have the latest data. Error: "
                              << linearizableReadStatus.reason()};
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    const bool wasEmpty = _taskLists[nss].empty();
    _taskLists[nss].addTask(std::move(task));

    if (wasEmpty) { //同一个表只会有一个task任务运行
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling"
                  << " will be attempted by the next caller to refresh this namespace.";
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _taskLists.erase(nss);
        }
        return status;
    }

    return Status::OK();
}

//
void ShardServerCatalogCacheLoader::_runTasks(const NamespaceString& nss) {
    auto context = _contexts.makeOperationContext(*Client::getCurrent());

    bool taskFinished = false;
    try {
		////把拿到的变化的collAndChunks信息更新到cache.chunks.库.表中
        _updatePersistedMetadata(context.opCtx(), nss);
        taskFinished = true;
    } catch (const DBException& ex) {
        Status exceptionStatus = ex.toStatus();

        // This thread must stop if we are shutting down
        if (ErrorCodes::isShutdownError(exceptionStatus.code())) {
            log() << "Failed to persist chunk metadata update for collection '" << nss
                  << "' due to shutdown.";
            return;
        }

        log() << redact(exceptionStatus);
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // If task completed successfully, remove it from work queue
    //更新本地cache chunks表结束，则剔除改任务
    if (taskFinished) {
        _taskLists[nss].pop_front();
    }

    // Schedule more work if there is any
    //如果该表还有其他任务，继续调度执行
    if (!_taskLists[nss].empty()) {
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule a persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling will be attempted by the next"
                  << " caller to refresh this namespace.";
            _taskLists.erase(nss);
        }
    } else {
        _taskLists.erase(nss);
    }
}

//ShardServerCatalogCacheLoader::_runTasks
//task对应的任务也就是该接口，把task对应的chunk更新到cache.chunks.库.表中
void ShardServerCatalogCacheLoader::_updatePersistedMetadata(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    const Task& task = _taskLists[nss].front();
    invariant(task.dropped || !task.collectionAndChangedChunks->changedChunks.empty());

    // If this task is from an old term and no longer valid, do not execute and return true so that
    // the task gets removed from the task list
    //主从状态发生变化直接返回
    if (task.termCreated != _term) {
        return;
    }

    lock.unlock();

    // Check if this is a drop task
    //说明表已经删除了
    //删除config.cache.collections中指定表的数据，同时删除config.cache.chunks.db.collection表
    if (task.dropped) {
        // The namespace was dropped. The persisted metadata for the collection must be cleared.
        Status status = dropChunksAndDeleteCollectionsEntry(opCtx, nss);
        uassert(status.code(),
                str::stream() << "Failed to clear persisted chunk metadata for collection '"
                              << nss.ns()
                              << "' due to '"
                              << status.reason()
                              << "'. Will be retried.",
                status.isOK());
        return;
    }

	////把拿到的变化的collAndChunks信息更新到cache.chunks.库.表中
    Status status =
        persistCollectionAndChangedChunks(opCtx, nss, task.collectionAndChangedChunks.get());

	//主节点刷路由过程中stepdown，会频繁打印该日志
	//Tue Feb 22 17:06:11.000 I SHARDING [ShardServerCatalogCacheLoader-0] PrimarySteppedDown: Failed to update the persisted chunk metadata for collection 'HDSS.MD_FCT_IER_DETAIL' from '0|0||000000000000000000000000' to '42277|54051||620cc3f8b8d642a2d9dcd717'. Will be retried. :: caused by :: Not primary while writing to config.cache.collections
	//Tue Feb 22 17:06:11.000 I SHARDING [ShardServerCatalogCacheLoader-6] PrimarySteppedDown: Failed to update the persisted chunk metadata for collection 'HDSS.MD_FCT_IER_DETAIL' from '0|0||000000000000000000000000' to '42277|54051||620cc3f8b8d642a2d9dcd717'. Will be retried. :: caused by :: Not primary while writing to config.cache.collections
	//Tue Feb 22 17:06:11.000 I SHARDING [ShardServerCatalogCacheLoader-0] PrimarySteppedDown: Failed to update the persisted chunk metadata for collection 'HDSS.MD_FCT_IER_DETAIL' from '0|0||000000000000000000000000' to '42277|54051||620cc3f8b8d642a2d9dcd717'. Will be retried. :: caused by :: Not primary while writing to config.cache.collections
    uassert(status.code(),
            str::stream() << "Failed to update the persisted chunk metadata for collection '"
                          << nss.ns()
                          << "' from '"
                          << task.minQueryVersion.toString()
                          << "' to '"
                          << task.maxQueryVersion.toString()
                          << "' due to '"
                          << status.reason()
                          << "'. Will be retried.",
            status.isOK());

    LOG(1) << "Successfully updated persisted chunk metadata for collection '" << nss << "' from '"
           << task.minQueryVersion << "' to collection version '" << task.maxQueryVersion << "'.";
}

CollectionAndChangedChunks
ShardServerCatalogCacheLoader::_getCompletePersistedMetadataForSecondarySinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkVersion& version) {
    // Keep trying to load the metadata until we get a complete view without updates being
    // concurrently applied.
    while (true) {
        const auto beginRefreshState = [&]() {
            while (true) {
                auto notif = _namespaceNotifications.createNotification(nss);

                auto refreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

                if (!refreshState.refreshing) {
                    return refreshState;
                }

                notif.get(opCtx);
            }
        }();

        // Load the metadata.
        CollectionAndChangedChunks collAndChangedChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version, true);

        // Check that no updates were concurrently applied while we were loading the metadata: this
        // could cause the loaded metadata to provide an incomplete view of the chunk ranges.
        const auto endRefreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

        if (beginRefreshState == endRefreshState) {
            return collAndChangedChunks;
        }

        LOG(1) << "Cache loader read meatadata while updates were being applied: this metadata may"
               << " be incomplete. Retrying. Refresh state before read: " << beginRefreshState
               << ". Current refresh state: '" << endRefreshState << "'.";
    }
}

//_schedulePrimaryGetChunksSince
ShardServerCatalogCacheLoader::Task::Task(
    StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
    ChunkVersion minimumQueryVersion,
    long long currentTerm)
    : taskNum(taskIdGenerator.fetchAndAdd(1)),
      minQueryVersion(minimumQueryVersion),
      termCreated(currentTerm) {
    if (statusWithCollectionAndChangedChunks.isOK()) {
        collectionAndChangedChunks = statusWithCollectionAndChangedChunks.getValue();
        invariant(!collectionAndChangedChunks->changedChunks.empty());
		//本次从config server获取到的增量chunk的最大版本信息
        maxQueryVersion = collectionAndChangedChunks->changedChunks.back().getVersion();
    } else {
    	//
        invariant(statusWithCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound);
        dropped = true;
        maxQueryVersion = ChunkVersion::UNSHARDED();
    }
}

ShardServerCatalogCacheLoader::TaskList::TaskList()
    : _activeTaskCompletedCondVar(std::make_shared<stdx::condition_variable>()) {}

void ShardServerCatalogCacheLoader::TaskList::addTask(Task task) {
    if (_tasks.empty()) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    if (task.dropped) {
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion));

        // As an optimization, on collection drop, clear any pending tasks in order to prevent any
        // throw-away work from executing. Because we have no way to differentiate whether the
        // active tasks is currently being operated on by a thread or not, we must leave the front
        // intact.
        _tasks.erase(std::next(_tasks.begin()), _tasks.end());

        // No need to schedule a drop if one is already currently active.
        if (!_tasks.front().dropped) {
            _tasks.emplace_back(std::move(task));
        }
    } else {
        // Tasks must have contiguous versions, unless a complete reload occurs.
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion) ||
                  !task.minQueryVersion.isSet());

        _tasks.emplace_back(std::move(task));
    }
}

void ShardServerCatalogCacheLoader::TaskList::pop_front() {
    invariant(!_tasks.empty());
    _tasks.pop_front();
    _activeTaskCompletedCondVar->notify_all();
}

//waitForCollectionFlush
void ShardServerCatalogCacheLoader::TaskList::waitForActiveTaskCompletion(
    stdx::unique_lock<stdx::mutex>& lg) {
    // Increase the use_count of the condition variable shared pointer, because the entire task list
    // might get deleted during the unlocked interval
    auto condVar = _activeTaskCompletedCondVar;
    condVar->wait(lg);
}

bool ShardServerCatalogCacheLoader::TaskList::hasTasksFromThisTerm(long long term) const {
    invariant(!_tasks.empty());
    return _tasks.back().termCreated == term;
}

ChunkVersion ShardServerCatalogCacheLoader::TaskList::getHighestVersionEnqueued() const {
    invariant(!_tasks.empty());
    return _tasks.back().maxQueryVersion;
}

//_getEnqueuedMetadata 
//把task中表的changed chuns信息添加到collAndChunks，实际上是针对指定表的指定term的，参考_getEnqueuedMetadata
CollectionAndChangedChunks ShardServerCatalogCacheLoader::TaskList::getEnqueuedMetadataForTerm(
    const long long term) const {
    CollectionAndChangedChunks collAndChunks;
    for (const auto& task : _tasks) {
        if (task.termCreated != term) {
            // Task data is no longer valid. Go on to the next task in the list.
            continue;
        }

        if (task.dropped) {
            // A drop task should reset the metadata.
            collAndChunks = CollectionAndChangedChunks();
        } else {
            //同一个表可能有多个task，因此这里可能会执行多次for循环，for的第一次进来走该分支
            if (task.collectionAndChangedChunks->epoch != collAndChunks.epoch) {
                // An epoch change should reset the metadata and start from the new.
                //这里会不会非常耗时，如果task.collectionAndChangedChunks中chunk非常多的情况?
                //epoch也会在这里赋值

				//多个task的epoch不一样，则以后面的task为准，这里直接替换
                collAndChunks = task.collectionAndChangedChunks.get();
            } else {
                // Epochs match, so the new results should be appended.

                // Make sure we do not append a duplicate chunk. The diff query is GTE, so there can
                // be duplicates of the same exact versioned chunk across tasks. This is no problem
                // for our diff application algorithms, but it can return unpredictable numbers of
                // chunks for testing purposes. Eliminate unpredicatable duplicates for testing
                // stability.
                auto taskCollectionAndChangedChunksIt =
                    task.collectionAndChangedChunks->changedChunks.begin();
                if (collAndChunks.changedChunks.back().getVersion() ==
                    task.collectionAndChangedChunks->changedChunks.front().getVersion()) {
                    ++taskCollectionAndChangedChunksIt;
                }

				//把task中表的changed chuns信息添加到collAndChunks
                collAndChunks.changedChunks.insert(
                    collAndChunks.changedChunks.end(),
                    taskCollectionAndChangedChunksIt,
                    task.collectionAndChangedChunks->changedChunks.end());
            }
        }
    }
    return collAndChunks;
}

}  // namespace mongo
