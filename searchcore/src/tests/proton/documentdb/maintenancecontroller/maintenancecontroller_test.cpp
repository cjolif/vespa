// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/document/repo/documenttyperepo.h>
#include <vespa/document/test/make_bucket_space.h>
#include <vespa/fastos/thread.h>
#include <vespa/config-attributes.h>
#include <vespa/searchcore/proton/attribute/attribute_config_inspector.h>
#include <vespa/searchcore/proton/attribute/attribute_usage_filter.h>
#include <vespa/searchcore/proton/attribute/i_attribute_manager.h>
#include <vespa/searchcore/proton/bucketdb/bucket_create_notifier.h>
#include <vespa/searchcore/proton/common/doctypename.h>
#include <vespa/searchcore/proton/common/feedtoken.h>
#include <vespa/searchcore/proton/common/transient_memory_usage_provider.h>
#include <vespa/searchcore/proton/documentmetastore/operation_listener.h>
#include <vespa/searchcore/proton/feedoperation/moveoperation.h>
#include <vespa/searchcore/proton/feedoperation/pruneremoveddocumentsoperation.h>
#include <vespa/searchcore/proton/feedoperation/putoperation.h>
#include <vespa/searchcore/proton/feedoperation/removeoperation.h>
#include <vespa/searchcore/proton/server/blockable_maintenance_job.h>
#include <vespa/searchcore/proton/server/executor_thread_service.h>
#include <vespa/searchcore/proton/server/i_operation_storer.h>
#include <vespa/searchcore/proton/server/ibucketmodifiedhandler.h>
#include <vespa/searchcore/proton/server/idocumentmovehandler.h>
#include <vespa/searchcore/proton/server/iheartbeathandler.h>
#include <vespa/searchcore/proton/server/ipruneremoveddocumentshandler.h>
#include <vespa/searchcore/proton/server/maintenance_controller_explorer.h>
#include <vespa/searchcore/proton/server/maintenance_jobs_injector.h>
#include <vespa/searchcore/proton/server/maintenancecontroller.h>
#include <vespa/searchcore/proton/test/buckethandler.h>
#include <vespa/searchcore/proton/test/clusterstatehandler.h>
#include <vespa/searchcore/proton/test/disk_mem_usage_notifier.h>
#include <vespa/searchcore/proton/test/mock_attribute_manager.h>
#include <vespa/searchcore/proton/test/test.h>
#include <vespa/searchlib/common/gatecallback.h>
#include <vespa/searchlib/common/idocumentmetastore.h>
#include <vespa/searchlib/index/docbuilder.h>
#include <vespa/vespalib/data/slime/slime.h>
#include <vespa/vespalib/testkit/testapp.h>
#include <vespa/vespalib/util/closuretask.h>
#include <vespa/vespalib/util/gate.h>
#include <vespa/vespalib/util/threadstackexecutor.h>
#include <unistd.h>

#include <vespa/log/log.h>
LOG_SETUP("maintenancecontroller_test");

using namespace proton;
using namespace vespalib::slime;
using document::BucketId;
using document::Document;
using document::DocumentId;
using document::test::makeBucketSpace;
using vespalib::system_clock;
using proton::bucketdb::BucketCreateNotifier;
using proton::matching::ISessionCachePruner;
using search::AttributeGuard;
using search::DocumentIdT;
using search::DocumentMetaData;
using search::IDestructorCallback;
using search::SerialNum;
using storage::spi::BucketInfo;
using storage::spi::Timestamp;
using vespalib::Slime;
using vespalib::makeClosure;
using vespalib::makeTask;
using vespa::config::search::AttributesConfigBuilder;

using BlockedReason = IBlockableMaintenanceJob::BlockedReason;

typedef BucketId::List BucketIdVector;
typedef std::set<BucketId> BucketIdSet;

constexpr int TIMEOUT_MS = 60000;
constexpr vespalib::duration TIMEOUT_SEC = 60s;

namespace {

void
sampleThreadId(FastOS_ThreadId *threadId)
{
    *threadId = FastOS_Thread::GetCurrentThreadId();
}

}  // namespace


class MyDocumentSubDB
{
    typedef std::map<DocumentIdT, Document::SP> DocMap;
    DocMap _docs;
    uint32_t _subDBId;
    DocumentMetaStore::SP _metaStoreSP;
    DocumentMetaStore & _metaStore;
    const std::shared_ptr<const document::DocumentTypeRepo> &_repo;
    const DocTypeName &_docTypeName;

public:
    MyDocumentSubDB(uint32_t subDBId, SubDbType subDbType, const std::shared_ptr<const document::DocumentTypeRepo> &repo,
                    std::shared_ptr<BucketDBOwner> bucketDB, const DocTypeName &docTypeName);
    ~MyDocumentSubDB();

    uint32_t getSubDBId() const { return _subDBId; }

    Document::UP
    getDocument(DocumentIdT lid) const
    {
        auto it(_docs.find(lid));
        if (it != _docs.end()) {
            return Document::UP(it->second->clone());
        } else {
            return Document::UP();
        }
    }

    MaintenanceDocumentSubDB getSubDB();
    void handlePruneRemovedDocuments(const PruneRemovedDocumentsOperation &op);
    void handlePut(PutOperation &op);
    void handleRemove(RemoveOperationWithDocId &op);
    void prepareMove(MoveOperation &op);
    void handleMove(const MoveOperation &op);
    uint32_t getNumUsedLids() const;
    uint32_t getDocumentCount() const { return _docs.size(); }

    void setBucketState(const BucketId &bucket, bool active) {
        _metaStore.setBucketState(bucket, active);
    }

    const IDocumentMetaStore &getMetaStore() const { return _metaStore; }
};

MyDocumentSubDB::MyDocumentSubDB(uint32_t subDBId, SubDbType subDbType, const std::shared_ptr<const document::DocumentTypeRepo> &repo,
                                 std::shared_ptr<BucketDBOwner> bucketDB, const DocTypeName &docTypeName)
    : _docs(),
      _subDBId(subDBId),
      _metaStoreSP(std::make_shared<DocumentMetaStore>(
              std::move(bucketDB), DocumentMetaStore::getFixedName(), search::GrowStrategy(),
              DocumentMetaStore::IGidCompare::SP(new DocumentMetaStore::DefaultGidCompare), subDbType)),
      _metaStore(*_metaStoreSP),
      _repo(repo),
      _docTypeName(docTypeName)
{
    _metaStore.constructFreeList();
}
MyDocumentSubDB::~MyDocumentSubDB() = default;

struct MyDocumentRetriever : public DocumentRetrieverBaseForTest
{
    MyDocumentSubDB &_subDB;

    explicit MyDocumentRetriever(MyDocumentSubDB &subDB) noexcept
        : _subDB(subDB)
    {
    }

    const document::DocumentTypeRepo &
    getDocumentTypeRepo() const override
    {
        LOG_ABORT("should not be reached");
    }

    void
    getBucketMetaData(const storage::spi::Bucket &,
                      DocumentMetaData::Vector &) const override
    {
        LOG_ABORT("should not be reached");
    }
    DocumentMetaData
    getDocumentMetaData(const DocumentId &) const override
    {
        return DocumentMetaData();
    }

    Document::UP
    getFullDocument(DocumentIdT lid) const override
    {
        return _subDB.getDocument(lid);
    }

    CachedSelect::SP
    parseSelect(const vespalib::string &) const override
    {
        return CachedSelect::SP();
    }
};


struct MyBucketModifiedHandler : public IBucketModifiedHandler
{
    BucketIdVector _modified;
    void notifyBucketModified(const BucketId &bucket) override {
        BucketIdVector::const_iterator itr = std::find(_modified.begin(), _modified.end(), bucket);
        if (itr == _modified.end()) {
            _modified.push_back(bucket);
        }
    }
    void reset() { _modified.clear(); }
};


struct MySessionCachePruner : public ISessionCachePruner
{
    bool isInvoked;
    MySessionCachePruner() : isInvoked(false) { }
    void pruneTimedOutSessions(vespalib::steady_time current) override {
        (void) current;
        isInvoked = true;
    }
};


class MyFeedHandler : public IDocumentMoveHandler,
                      public IPruneRemovedDocumentsHandler,
                      public IHeartBeatHandler,
                      public IOperationStorer
{
    FastOS_ThreadId                _executorThreadId;
    std::vector<MyDocumentSubDB *> _subDBs;
    SerialNum                      _serialNum;
    uint32_t                       _heartBeats;
public:
    explicit MyFeedHandler(FastOS_ThreadId &executorThreadId);

    ~MyFeedHandler() override;

    bool isExecutorThread() const;
    void handleMove(MoveOperation &op, IDestructorCallback::SP moveDoneCtx) override;
    void performPruneRemovedDocuments(PruneRemovedDocumentsOperation &op) override;
    void heartBeat() override;

    void setSubDBs(const std::vector<MyDocumentSubDB *> &subDBs);

    SerialNum incSerialNum() {
        return ++_serialNum;
    }

    // Implements IOperationStorer
    void appendOperation(const FeedOperation &op, DoneCallback) override;
    CommitResult startCommit(DoneCallback) override {
        return CommitResult();
    }

    uint32_t getHeartBeats() const {
        return _heartBeats;
    }
};


class MyExecutor: public vespalib::ThreadStackExecutor
{
public:
    FastOS_ThreadId       _threadId;

    MyExecutor();

    ~MyExecutor() override;

    bool isIdle();
    bool waitIdle(vespalib::duration timeout);
};


class MyFrozenBucket
{
    IBucketFreezer &_freezer;
    BucketId _bucketId;
public:
    typedef std::unique_ptr<MyFrozenBucket> UP;

    MyFrozenBucket(IBucketFreezer &freezer,
                   const BucketId &bucketId)
        : _freezer(freezer),
          _bucketId(bucketId)
    {
        _freezer.freezeBucket(_bucketId);
    }

    ~MyFrozenBucket()
    {
        _freezer.thawBucket(_bucketId);
    }
};

struct MySimpleJob : public BlockableMaintenanceJob
{
    vespalib::CountDownLatch _latch;
    size_t                   _runCnt;

    MySimpleJob(vespalib::duration delay,
                vespalib::duration interval,
                uint32_t finishCount)
        : BlockableMaintenanceJob("my_job", delay, interval),
          _latch(finishCount),
          _runCnt(0)
    {
    }
    void block() { setBlocked(BlockedReason::FROZEN_BUCKET); }
    bool run() override {
        LOG(info, "MySimpleJob::run()");
        _latch.countDown();
        ++_runCnt;
        return true;
    }
};

struct MySplitJob : public MySimpleJob
{
    MySplitJob(vespalib::duration delay,
               vespalib::duration interval,
               uint32_t finishCount)
        : MySimpleJob(delay, interval, finishCount)
    {
    }
    bool run() override {
        LOG(info, "MySplitJob::run()");
        _latch.countDown();
        ++_runCnt;
        return _latch.getCount() == 0;
    }
};

struct MyLongRunningJob : public BlockableMaintenanceJob
{
    vespalib::Gate _firstRun;

    MyLongRunningJob(vespalib::duration delay,
                     vespalib::duration interval)
        : BlockableMaintenanceJob("long_running_job", delay, interval),
          _firstRun()
    {
    }
    void block() { setBlocked(BlockedReason::FROZEN_BUCKET); }
    bool run() override {
        _firstRun.countDown();
        usleep(10000);
        return false;
    }
};

using MyAttributeManager = test::MockAttributeManager;

struct MockLidSpaceCompactionHandler : public ILidSpaceCompactionHandler
{
    vespalib::string name;

    explicit MockLidSpaceCompactionHandler(const vespalib::string &name_) : name(name_) {}
    vespalib::string getName() const override { return name; }
    void set_operation_listener(documentmetastore::OperationListener::SP) override {}
    uint32_t getSubDbId() const override { return 0; }
    search::LidUsageStats getLidStatus() const override { return search::LidUsageStats(); }
    IDocumentScanIterator::UP getIterator() const override { return IDocumentScanIterator::UP(); }
    MoveOperation::UP createMoveOperation(const search::DocumentMetaData &, uint32_t) const override { return MoveOperation::UP(); }
    void handleMove(const MoveOperation &, IDestructorCallback::SP) override {}
    void handleCompactLidSpace(const CompactLidSpaceOperation &, std::shared_ptr<IDestructorCallback>) override {}
};


class MaintenanceControllerFixture : public ICommitable
{
public:
    MyExecutor                    _executor;
    MyExecutor                    _genericExecutor;
    ExecutorThreadService         _threadService;
    DocTypeName                   _docTypeName;
    test::UserDocumentsBuilder    _builder;
    std::shared_ptr<BucketDBOwner> _bucketDB;
    test::BucketStateCalculator::SP _calc;
    test::ClusterStateHandler     _clusterStateHandler;
    test::BucketHandler           _bucketHandler;
    MyBucketModifiedHandler       _bmc;
    MyDocumentSubDB               _ready;
    MyDocumentSubDB               _removed;
    MyDocumentSubDB               _notReady;
    MySessionCachePruner          _gsp;
    MyFeedHandler                 _fh;
    ILidSpaceCompactionHandler::Vector _lscHandlers;
    DocumentDBMaintenanceConfig::SP _mcCfg;
    bool                          _injectDefaultJobs;
    DocumentDBJobTrackers         _jobTrackers;
    std::shared_ptr<proton::IAttributeManager> _readyAttributeManager;
    std::shared_ptr<proton::IAttributeManager> _notReadyAttributeManager;
    AttributeUsageFilter          _attributeUsageFilter;
    test::DiskMemUsageNotifier    _diskMemUsageNotifier;
    BucketCreateNotifier          _bucketCreateNotifier;
    MaintenanceController         _mc;

    MaintenanceControllerFixture();

    ~MaintenanceControllerFixture() override;

    void syncSubDBs();
    void commit() override { }
    void commitAndWait(ILidCommitState & ) override { }
    void commitAndWait(ILidCommitState & tracker, uint32_t ) override {
        commitAndWait(tracker);
    }
    void commitAndWait(ILidCommitState & tracker, const std::vector<uint32_t> & ) override {
        commitAndWait(tracker);
    }
    void performSyncSubDBs();
    void notifyClusterStateChanged();
    void performNotifyClusterStateChanged();
    void startMaintenance();
    void injectMaintenanceJobs();
    void performStartMaintenance();
    void stopMaintenance();
    void forwardMaintenanceConfig();
    void performForwardMaintenanceConfig();

    void insertDocs(const test::UserDocuments &docs, MyDocumentSubDB &subDb);

    void removeDocs(const test::UserDocuments &docs, Timestamp timestamp);

    void
    setPruneConfig(const DocumentDBPruneRemovedDocumentsConfig &pruneConfig)
    {
        auto newCfg = std::make_shared<DocumentDBMaintenanceConfig>(
                           pruneConfig,
                           _mcCfg->getHeartBeatConfig(),
                           _mcCfg->getSessionCachePruneInterval(),
                           _mcCfg->getVisibilityDelay(),
                           _mcCfg->getLidSpaceCompactionConfig(),
                           _mcCfg->getAttributeUsageFilterConfig(),
                           _mcCfg->getAttributeUsageSampleInterval(),
                           _mcCfg->getBlockableJobConfig(),
                           _mcCfg->getFlushConfig());
        _mcCfg = newCfg;
        forwardMaintenanceConfig();
    }

    void
    setHeartBeatConfig(const DocumentDBHeartBeatConfig &heartBeatConfig)
    {
        auto newCfg = std::make_shared<DocumentDBMaintenanceConfig>(
                           _mcCfg->getPruneRemovedDocumentsConfig(),
                           heartBeatConfig,
                           _mcCfg->getSessionCachePruneInterval(),
                           _mcCfg->getVisibilityDelay(),
                           _mcCfg->getLidSpaceCompactionConfig(),
                           _mcCfg->getAttributeUsageFilterConfig(),
                           _mcCfg->getAttributeUsageSampleInterval(),
                           _mcCfg->getBlockableJobConfig(),
                           _mcCfg->getFlushConfig());
        _mcCfg = newCfg;
        forwardMaintenanceConfig();
    }

    void
    setGroupingSessionPruneInterval(vespalib::duration groupingSessionPruneInterval)
    {
        auto newCfg = std::make_shared<DocumentDBMaintenanceConfig>(
                           _mcCfg->getPruneRemovedDocumentsConfig(),
                           _mcCfg->getHeartBeatConfig(),
                           groupingSessionPruneInterval,
                           _mcCfg->getVisibilityDelay(),
                           _mcCfg->getLidSpaceCompactionConfig(),
                           _mcCfg->getAttributeUsageFilterConfig(),
                           _mcCfg->getAttributeUsageSampleInterval(),
                           _mcCfg->getBlockableJobConfig(),
                           _mcCfg->getFlushConfig());
        _mcCfg = newCfg;
        forwardMaintenanceConfig();
    }

    void setLidSpaceCompactionConfig(const DocumentDBLidSpaceCompactionConfig &cfg) {
        auto newCfg = std::make_shared<DocumentDBMaintenanceConfig>(
                           _mcCfg->getPruneRemovedDocumentsConfig(),
                           _mcCfg->getHeartBeatConfig(),
                           _mcCfg->getSessionCachePruneInterval(),
                           _mcCfg->getVisibilityDelay(),
                           cfg,
                           _mcCfg->getAttributeUsageFilterConfig(),
                           _mcCfg->getAttributeUsageSampleInterval(),
                           _mcCfg->getBlockableJobConfig(),
                           _mcCfg->getFlushConfig());
        _mcCfg = newCfg;
        forwardMaintenanceConfig();
    }

    void
    performNotifyBucketStateChanged(document::BucketId bucketId, BucketInfo::ActiveState newState)
    {
        _bucketHandler.notifyBucketStateChanged(bucketId, newState);
    }

    void
    notifyBucketStateChanged(const document::BucketId &bucketId, BucketInfo::ActiveState newState)
    {
        _executor.execute(makeTask(makeClosure(this,
                                               &MaintenanceControllerFixture::
                                               performNotifyBucketStateChanged,
                                               bucketId, newState)));
        _executor.sync();
    }
};


MaintenanceDocumentSubDB
MyDocumentSubDB::getSubDB()
{
    auto retriever = std::make_shared<MyDocumentRetriever>(*this);

    return MaintenanceDocumentSubDB("my_sub_db", _subDBId,
                                    _metaStoreSP,
                                    retriever,
                                    IFeedView::SP());
}


void
MyDocumentSubDB::handlePruneRemovedDocuments(const PruneRemovedDocumentsOperation &op)
{
    assert(_subDBId == 1u);
    typedef LidVectorContext::LidVector LidVector;
    const SerialNum serialNum = op.getSerialNum();
    const LidVectorContext &lidCtx = *op.getLidsToRemove();
    const LidVector &lidsToRemove(lidCtx.getLidVector());
    _metaStore.removeBatch(lidsToRemove, lidCtx.getDocIdLimit());
    _metaStore.removeBatchComplete(lidsToRemove);
    _metaStore.commit(serialNum);
    for (auto lid : lidsToRemove) {
        _docs.erase(lid);
    }
}


void
MyDocumentSubDB::handlePut(PutOperation &op)
{
    const SerialNum serialNum = op.getSerialNum();
    const Document::SP &doc = op.getDocument();
    const DocumentId &docId = doc->getId();
    const document::GlobalId &gid = docId.getGlobalId();
    bool needCommit = false;

    if (op.getValidDbdId(_subDBId)) {
        typedef DocumentMetaStore::Result PutRes;

        PutRes putRes(_metaStore.put(gid,
                                     op.getBucketId(),
                                     op.getTimestamp(),
                                     op.getSerializedDocSize(),
                                     op.getLid(), 0u));
        assert(putRes.ok());
        assert(op.getLid() == putRes._lid);
        _docs[op.getLid()] = doc;
        needCommit = true;
    }
    if (op.getValidPrevDbdId(_subDBId) && op.changedDbdId()) {
        assert(_metaStore.validLid(op.getPrevLid()));
        const RawDocumentMetaData &meta(_metaStore.getRawMetaData(op.getPrevLid()));
        assert((_subDBId == 1u) == op.getPrevMarkedAsRemoved());
        assert(meta.getGid() == gid);
        (void) meta;

        bool remres = _metaStore.remove(op.getPrevLid(), 0u);
        assert(remres);
        (void) remres;
        _metaStore.removeComplete(op.getPrevLid());

        _docs.erase(op.getPrevLid());
        needCommit = true;
    }
    if (needCommit) {
        _metaStore.commit(serialNum, serialNum);
    }
}


void
MyDocumentSubDB::handleRemove(RemoveOperationWithDocId &op)
{
    const SerialNum serialNum = op.getSerialNum();
    const DocumentId &docId = op.getDocumentId();
    const document::GlobalId &gid = op.getGlobalId();
    bool needCommit = false;

    if (op.getValidDbdId(_subDBId)) {
        typedef DocumentMetaStore::Result PutRes;

        PutRes putRes(_metaStore.put(gid,
                                     op.getBucketId(),
                                     op.getTimestamp(),
                                     op.getSerializedDocSize(),
                                     op.getLid(), 0u));
        assert(putRes.ok());
        assert(op.getLid() == putRes._lid);
        const document::DocumentType *docType =
            _repo->getDocumentType(_docTypeName.getName());
        auto doc = std::make_unique<Document>(*docType, docId);
        doc->setRepo(*_repo);
        _docs[op.getLid()] = std::move(doc);
        needCommit = true;
    }
    if (op.getValidPrevDbdId(_subDBId) && op.changedDbdId()) {
        assert(_metaStore.validLid(op.getPrevLid()));
        const RawDocumentMetaData &meta(_metaStore.getRawMetaData(op.getPrevLid()));
        assert((_subDBId == 1u) == op.getPrevMarkedAsRemoved());
        assert(meta.getGid() == gid);
        (void) meta;

        bool remres = _metaStore.remove(op.getPrevLid(), 0u);
        assert(remres);
        (void) remres;

        _metaStore.removeComplete(op.getPrevLid());
        _docs.erase(op.getPrevLid());
        needCommit = true;
    }
    if (needCommit) {
        _metaStore.commit(serialNum, serialNum);
    }
}


void
MyDocumentSubDB::prepareMove(MoveOperation &op)
{
    const DocumentId &docId = op.getDocument()->getId();
    const document::GlobalId &gid = docId.getGlobalId();
    DocumentMetaStore::Result inspectResult = _metaStore.inspect(gid, 0u);
    assert(!inspectResult._found);
    op.setDbDocumentId(DbDocumentId(_subDBId, inspectResult._lid));
}


void
MyDocumentSubDB::handleMove(const MoveOperation &op)
{
    const SerialNum serialNum = op.getSerialNum();
    const Document::SP &doc = op.getDocument();
    const DocumentId &docId = doc->getId();
    const document::GlobalId &gid = docId.getGlobalId();
    bool needCommit = false;

    if (op.getValidDbdId(_subDBId)) {
        typedef DocumentMetaStore::Result PutRes;

        PutRes putRes(_metaStore.put(gid,
                                     op.getBucketId(),
                                     op.getTimestamp(),
                                     op.getSerializedDocSize(),
                                     op.getLid(), 0u));
        assert(putRes.ok());
        assert(op.getLid() == putRes._lid);
        _docs[op.getLid()] = doc;
        needCommit = true;
    }
    if (op.getValidPrevDbdId(_subDBId)) {
        assert(_metaStore.validLid(op.getPrevLid()));
        const RawDocumentMetaData &meta(_metaStore.getRawMetaData(op.getPrevLid()));
        assert((_subDBId == 1u) == op.getPrevMarkedAsRemoved());
        assert(meta.getGid() == gid);
        (void) meta;

        bool remres = _metaStore.remove(op.getPrevLid(), 0u);
        assert(remres);
        (void) remres;

        _metaStore.removeComplete(op.getPrevLid());
        _docs.erase(op.getPrevLid());
        needCommit = true;
    }
    if (needCommit) {
        _metaStore.commit(serialNum, serialNum);
    }
}


uint32_t
MyDocumentSubDB::getNumUsedLids() const
{
    return _metaStore.getNumUsedLids();
}


MyFeedHandler::MyFeedHandler(FastOS_ThreadId &executorThreadId)
    : IDocumentMoveHandler(),
      IPruneRemovedDocumentsHandler(),
      IHeartBeatHandler(),
      _executorThreadId(executorThreadId),
      _subDBs(),
      _serialNum(0u),
      _heartBeats(0u)
{
}


MyFeedHandler::~MyFeedHandler() = default;


bool
MyFeedHandler::isExecutorThread() const
{
    FastOS_ThreadId threadId(FastOS_Thread::GetCurrentThreadId());
    return FastOS_Thread::CompareThreadIds(_executorThreadId, threadId);
}


void
MyFeedHandler::handleMove(MoveOperation &op, IDestructorCallback::SP moveDoneCtx)
{
    assert(isExecutorThread());
    assert(op.getValidPrevDbdId());
    _subDBs[op.getSubDbId()]->prepareMove(op);
    assert(op.getValidDbdId());
    assert(op.getSubDbId() != op.getPrevSubDbId());
    // Check for wrong magic numbers
    assert(op.getSubDbId() != 1u);
    assert(op.getPrevSubDbId() != 1u);
    assert(op.getSubDbId() < _subDBs.size());
    assert(op.getPrevSubDbId() < _subDBs.size());
    appendOperation(op, std::move(moveDoneCtx));
    _subDBs[op.getSubDbId()]->handleMove(op);
    _subDBs[op.getPrevSubDbId()]->handleMove(op);
}


void
MyFeedHandler::performPruneRemovedDocuments(PruneRemovedDocumentsOperation &op)
{
    assert(isExecutorThread());
    if (op.getLidsToRemove()->getNumLids() != 0u) {
        appendOperation(op, std::make_shared<search::IgnoreCallback>());
        // magic number.
        _subDBs[1u]->handlePruneRemovedDocuments(op);
    }
}


void
MyFeedHandler::heartBeat()
{
    assert(isExecutorThread());
    ++_heartBeats;
}


void
MyFeedHandler::setSubDBs(const std::vector<MyDocumentSubDB *> &subDBs)
{
    _subDBs = subDBs;
}


void
MyFeedHandler::appendOperation(const FeedOperation &op, DoneCallback)
{
    const_cast<FeedOperation &>(op).setSerialNum(incSerialNum());
}


MyExecutor::MyExecutor()
    : vespalib::ThreadStackExecutor(1, 128 * 1024),
      _threadId()
{
    execute(makeTask(makeClosure(&sampleThreadId, &_threadId)));
    sync();
}


MyExecutor::~MyExecutor() = default;


bool
MyExecutor::isIdle()
{
    (void) getStats();
    sync();
    Stats stats(getStats());
    return stats.acceptedTasks == 0u;
}


bool
MyExecutor::waitIdle(vespalib::duration timeout)
{
    vespalib::Timer timer;
    while (!isIdle()) {
        if (timer.elapsed() >= timeout)
            return false;
    }
    return true;
}


MaintenanceControllerFixture::MaintenanceControllerFixture()
    : _executor(),
      _genericExecutor(),
      _threadService(_executor),
      _docTypeName("searchdocument"), // must match document builder
      _builder(),
      _bucketDB(std::make_shared<BucketDBOwner>()),
      _calc(new test::BucketStateCalculator()),
      _clusterStateHandler(),
      _bucketHandler(),
      _bmc(),
      _ready(0u, SubDbType::READY, _builder.getRepo(), _bucketDB, _docTypeName),
      _removed(1u, SubDbType::REMOVED, _builder.getRepo(), _bucketDB,
               _docTypeName),
      _notReady(2u, SubDbType::NOTREADY, _builder.getRepo(), _bucketDB,
                _docTypeName),
      _gsp(),
      _fh(_executor._threadId),
      _lscHandlers(),
      _mcCfg(new DocumentDBMaintenanceConfig),
      _injectDefaultJobs(true),
      _jobTrackers(),
      _readyAttributeManager(std::make_shared<MyAttributeManager>()),
      _notReadyAttributeManager(std::make_shared<MyAttributeManager>()),
      _attributeUsageFilter(),
      _bucketCreateNotifier(),
      _mc(_threadService, _genericExecutor, _docTypeName)
{
    std::vector<MyDocumentSubDB *> subDBs;
    subDBs.push_back(&_ready);
    subDBs.push_back(&_removed);
    subDBs.push_back(&_notReady);
    _fh.setSubDBs(subDBs);
    syncSubDBs();
}


MaintenanceControllerFixture::~MaintenanceControllerFixture()
{
    stopMaintenance();
}


void
MaintenanceControllerFixture::syncSubDBs()
{
    _executor.execute(makeTask(makeClosure(this,
                                       &MaintenanceControllerFixture::
                                       performSyncSubDBs)));
    _executor.sync();
}


void
MaintenanceControllerFixture::performSyncSubDBs()
{
    _mc.syncSubDBs(_ready.getSubDB(),
                   _removed.getSubDB(),
                   _notReady.getSubDB());
}


void
MaintenanceControllerFixture::notifyClusterStateChanged()
{
    _executor.execute(makeTask(makeClosure(this,
                                       &MaintenanceControllerFixture::
                                       performNotifyClusterStateChanged)));
    _executor.sync();
}


void
MaintenanceControllerFixture::performNotifyClusterStateChanged()
{
    _clusterStateHandler.notifyClusterStateChanged(_calc);
}


void
MaintenanceControllerFixture::startMaintenance()
{
    _executor.execute(makeTask(makeClosure(this,
                                       &MaintenanceControllerFixture::
                                       performStartMaintenance)));
    _executor.sync();
}

void
MaintenanceControllerFixture::injectMaintenanceJobs()
{
    if (_injectDefaultJobs) {
        MaintenanceJobsInjector::injectJobs(_mc, *_mcCfg, _fh, _gsp,
                                            _lscHandlers, _fh, _mc, _bucketCreateNotifier, _docTypeName.getName(), makeBucketSpace(),
                                            _fh, _fh, _bmc, _clusterStateHandler, _bucketHandler,
                                            _calc,
                                            _diskMemUsageNotifier,
                                            _jobTrackers, *this,
                                            _readyAttributeManager,
                                            _notReadyAttributeManager,
                                            std::make_unique<const AttributeConfigInspector>(AttributesConfigBuilder()),
                                            std::make_shared<TransientMemoryUsageProvider>(),
                                            _attributeUsageFilter);
    }
}

void
MaintenanceControllerFixture::performStartMaintenance()
{
    injectMaintenanceJobs();
    _mc.start(_mcCfg);
}


void
MaintenanceControllerFixture::stopMaintenance()
{
    _mc.stop();
    _executor.sync();
}


void
MaintenanceControllerFixture::forwardMaintenanceConfig()
{
    _executor.execute(makeTask(makeClosure(this,
                                       &MaintenanceControllerFixture::
                                       performForwardMaintenanceConfig)));
    _executor.sync();
}


void
MaintenanceControllerFixture::performForwardMaintenanceConfig()
{
    _mc.killJobs();
    injectMaintenanceJobs();
    _mc.newConfig(_mcCfg);
}


void
MaintenanceControllerFixture::insertDocs(const test::UserDocuments &docs, MyDocumentSubDB &subDb)
{

    for (const auto & entry : docs) {
        const test::BucketDocuments &bucketDocs = entry.second;
        for (const test::Document &testDoc : bucketDocs.getDocs()) {
            PutOperation op(testDoc.getBucket(), testDoc.getTimestamp(), testDoc.getDoc());
            op.setDbDocumentId(DbDocumentId(subDb.getSubDBId(), testDoc.getLid()));
            _fh.appendOperation(op, std::make_shared<search::IgnoreCallback>());
            subDb.handlePut(op);
        }
    }
}


void
MaintenanceControllerFixture::removeDocs(const test::UserDocuments &docs, Timestamp timestamp)
{

    for (const auto & entry : docs) {
        const test::BucketDocuments &bucketDocs = entry.second;
        for (const test::Document &testDoc : bucketDocs.getDocs()) {
            RemoveOperationWithDocId op(testDoc.getBucket(), timestamp, testDoc.getDoc()->getId());
            op.setDbDocumentId(DbDocumentId(_removed.getSubDBId(), testDoc.getLid()));
            _fh.appendOperation(op, std::make_shared<search::IgnoreCallback>());
            _removed.handleRemove(op);
        }
    }
}

TEST_F("require that bucket move controller is active",
       MaintenanceControllerFixture)
{
    f._builder.createDocs(1, 1, 4); // 3 docs
    f._builder.createDocs(2, 4, 6); // 2 docs
    test::UserDocuments readyDocs(f._builder.getDocs());
    BucketId bucketId1(readyDocs.getBucket(1));
    BucketId bucketId2(readyDocs.getBucket(2));
    f.insertDocs(readyDocs, f._ready);
    f._builder.clearDocs();
    f._builder.createDocs(3, 1, 3); // 2 docs
    f._builder.createDocs(4, 3, 6); // 3 docs
    test::UserDocuments notReadyDocs(f._builder.getDocs());
    BucketId bucketId4(notReadyDocs.getBucket(4));
    f.insertDocs(notReadyDocs, f._notReady);
    f._builder.clearDocs();
    f.notifyClusterStateChanged();
    EXPECT_TRUE(f._executor.isIdle());
    EXPECT_EQUAL(5u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(5u, f._ready.getDocumentCount());
    EXPECT_EQUAL(5u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(5u, f._notReady.getDocumentCount());
    f.startMaintenance();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(0u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(0u, f._ready.getDocumentCount());
    EXPECT_EQUAL(10u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(10u, f._notReady.getDocumentCount());
    f._calc->addReady(bucketId1);
    f.notifyClusterStateChanged();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(3u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(3u, f._ready.getDocumentCount());
    EXPECT_EQUAL(7u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(7u, f._notReady.getDocumentCount());
    MyFrozenBucket::UP frozen2(new MyFrozenBucket(f._mc, bucketId2));
    f._calc->addReady(bucketId2);
    f._calc->addReady(bucketId4);
    f.notifyClusterStateChanged();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(6u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(6u, f._ready.getDocumentCount());
    EXPECT_EQUAL(4u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(4u, f._notReady.getDocumentCount());
    frozen2.reset();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(8u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(8u, f._ready.getDocumentCount());
    EXPECT_EQUAL(2u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(2u, f._notReady.getDocumentCount());
}

TEST_F("require that document pruner is active",
       MaintenanceControllerFixture)
{
    uint64_t tshz = 1000000;
    uint64_t now = static_cast<uint64_t>(time(nullptr)) * tshz;
    Timestamp remTime(static_cast<Timestamp::Type>(now - 3600 * tshz));
    Timestamp keepTime(static_cast<Timestamp::Type>(now + 3600 * tshz));
    f._builder.createDocs(1, 1, 4); // 3 docs
    f._builder.createDocs(2, 4, 6); // 2 docs
    test::UserDocuments keepDocs(f._builder.getDocs());
    f.removeDocs(keepDocs, keepTime);
    f._builder.clearDocs();
    f._builder.createDocs(3, 6, 8); // 2 docs
    f._builder.createDocs(4, 8, 11); // 3 docs
    test::UserDocuments removeDocs(f._builder.getDocs());
    BucketId bucketId3(removeDocs.getBucket(3));
    f.removeDocs(removeDocs, remTime);
    f.notifyClusterStateChanged();
    EXPECT_TRUE(f._executor.isIdle());
    EXPECT_EQUAL(10u, f._removed.getNumUsedLids());
    EXPECT_EQUAL(10u, f._removed.getDocumentCount());
    f.startMaintenance();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(10u, f._removed.getNumUsedLids());
    EXPECT_EQUAL(10u, f._removed.getDocumentCount());
    MyFrozenBucket::UP frozen3(new MyFrozenBucket(f._mc, bucketId3));
    f.setPruneConfig(DocumentDBPruneRemovedDocumentsConfig(200ms, 900s));
    for (uint32_t i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(100ms);
        ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
        if (f._removed.getNumUsedLids() != 10u)
            break;
    }
    EXPECT_EQUAL(10u, f._removed.getNumUsedLids());
    EXPECT_EQUAL(10u, f._removed.getDocumentCount());
    frozen3.reset();
    for (uint32_t i = 0; i < 600; ++i) {
        std::this_thread::sleep_for(100ms);
        ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
        if (f._removed.getNumUsedLids() != 10u)
            break;
    }
    EXPECT_EQUAL(5u, f._removed.getNumUsedLids());
    EXPECT_EQUAL(5u, f._removed.getDocumentCount());
}

TEST_F("require that heartbeats are scheduled",
       MaintenanceControllerFixture)
{
    f.notifyClusterStateChanged();
    f.startMaintenance();
    f.setHeartBeatConfig(DocumentDBHeartBeatConfig(200ms));
    for (uint32_t i = 0; i < 600; ++i) {
        std::this_thread::sleep_for(100ms);
        if (f._fh.getHeartBeats() != 0u)
            break;
    }
    EXPECT_GREATER(f._fh.getHeartBeats(), 0u);
}

TEST_F("require that periodic session prunings are scheduled",
       MaintenanceControllerFixture)
{
    ASSERT_FALSE(f._gsp.isInvoked);
    f.notifyClusterStateChanged();
    f.startMaintenance();
    f.setGroupingSessionPruneInterval(200ms);
    for (uint32_t i = 0; i < 600; ++i) {
        std::this_thread::sleep_for(100ms);
        if (f._gsp.isInvoked) {
            break;
        }
    }
    ASSERT_TRUE(f._gsp.isInvoked);
}

TEST_F("require that active bucket is not moved until de-activated", MaintenanceControllerFixture)
{
    f._builder.createDocs(1, 1, 4); // 3 docs
    f._builder.createDocs(2, 4, 6); // 2 docs
    test::UserDocuments readyDocs(f._builder.getDocs());
    f.insertDocs(readyDocs, f._ready);
    f._builder.clearDocs();
    f._builder.createDocs(3, 1, 3); // 2 docs
    f._builder.createDocs(4, 3, 6); // 3 docs
    test::UserDocuments notReadyDocs(f._builder.getDocs());
    f.insertDocs(notReadyDocs, f._notReady);
    f._builder.clearDocs();

    // bucket 1 (active) should be moved from ready to not ready according to cluster state
    f._calc->addReady(readyDocs.getBucket(2));
    f._ready.setBucketState(readyDocs.getBucket(1), true);

    f.notifyClusterStateChanged();
    EXPECT_TRUE(f._executor.isIdle());
    EXPECT_EQUAL(5u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(5u, f._ready.getDocumentCount());
    EXPECT_EQUAL(5u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(5u, f._notReady.getDocumentCount());

    f.startMaintenance();
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(5u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(5u, f._ready.getDocumentCount());
    EXPECT_EQUAL(5u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(5u, f._notReady.getDocumentCount());

    // de-activate bucket 1
    f._ready.setBucketState(readyDocs.getBucket(1), false);
    f.notifyBucketStateChanged(readyDocs.getBucket(1), BucketInfo::NOT_ACTIVE);
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(2u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(2u, f._ready.getDocumentCount());
    EXPECT_EQUAL(8u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(8u, f._notReady.getDocumentCount());

    // re-activate bucket 1
    f._ready.setBucketState(readyDocs.getBucket(1), true);
    f.notifyBucketStateChanged(readyDocs.getBucket(1), BucketInfo::ACTIVE);
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(5u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(5u, f._ready.getDocumentCount());
    EXPECT_EQUAL(5u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(5u, f._notReady.getDocumentCount());

    // de-activate bucket 1
    f._ready.setBucketState(readyDocs.getBucket(1), false);
    f.notifyBucketStateChanged(readyDocs.getBucket(1), BucketInfo::NOT_ACTIVE);
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(2u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(2u, f._ready.getDocumentCount());
    EXPECT_EQUAL(8u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(8u, f._notReady.getDocumentCount());

    // re-activate bucket 1
    f._ready.setBucketState(readyDocs.getBucket(1), true);
    f.notifyBucketStateChanged(readyDocs.getBucket(1), BucketInfo::ACTIVE);
    ASSERT_TRUE(f._executor.waitIdle(TIMEOUT_SEC));
    EXPECT_EQUAL(5u, f._ready.getNumUsedLids());
    EXPECT_EQUAL(5u, f._ready.getDocumentCount());
    EXPECT_EQUAL(5u, f._notReady.getNumUsedLids());
    EXPECT_EQUAL(5u, f._notReady.getDocumentCount());
}

TEST_F("require that a simple maintenance job is executed", MaintenanceControllerFixture)
{
    auto job = std::make_unique<MySimpleJob>(200ms, 200ms, 3);
    MySimpleJob &myJob = *job;
    f._mc.registerJobInMasterThread(std::move(job));
    f._injectDefaultJobs = false;
    f.startMaintenance();
    bool done = myJob._latch.await(TIMEOUT_MS);
    EXPECT_TRUE(done);
    EXPECT_EQUAL(0u, myJob._latch.getCount());
}

TEST_F("require that a split maintenance job is executed", MaintenanceControllerFixture)
{
    auto job = std::make_unique<MySplitJob>(200ms, TIMEOUT_SEC * 2, 3);
    MySplitJob &myJob = *job;
    f._mc.registerJobInMasterThread(std::move(job));
    f._injectDefaultJobs = false;
    f.startMaintenance();
    bool done = myJob._latch.await(TIMEOUT_MS);
    EXPECT_TRUE(done);
    EXPECT_EQUAL(0u, myJob._latch.getCount());
}

TEST_F("require that a blocked job is unblocked and executed after thaw bucket",
        MaintenanceControllerFixture)
{
    auto job1 = std::make_unique<MySimpleJob>(TIMEOUT_SEC * 2, TIMEOUT_SEC * 2, 1);
    MySimpleJob &myJob1 = *job1;
    auto job2 = std::make_unique< MySimpleJob>(TIMEOUT_SEC * 2, TIMEOUT_SEC * 2, 0);
    MySimpleJob &myJob2 = *job2;
    f._mc.registerJobInMasterThread(std::move(job1));
    f._mc.registerJobInMasterThread(std::move(job2));
    f._injectDefaultJobs = false;
    f.startMaintenance();

    myJob1.block();
    EXPECT_TRUE(myJob1.isBlocked());
    EXPECT_FALSE(myJob2.isBlocked());
    IBucketFreezer &ibf = f._mc;
    ibf.freezeBucket(BucketId(1));
    ibf.thawBucket(BucketId(1));
    EXPECT_TRUE(myJob1.isBlocked());
    ibf.freezeBucket(BucketId(1));
    IFrozenBucketHandler & fbh = f._mc;
    // This is to simulate contention, as that is required for notification on thawed buckets.
    EXPECT_FALSE(fbh.acquireExclusiveBucket(BucketId(1)));
    ibf.thawBucket(BucketId(1));
    f._executor.sync();
    EXPECT_FALSE(myJob1.isBlocked());
    EXPECT_FALSE(myJob2.isBlocked());
    bool done1 = myJob1._latch.await(TIMEOUT_MS);
    EXPECT_TRUE(done1);
    std::this_thread::sleep_for(2s);
    EXPECT_EQUAL(0u, myJob2._runCnt);
}

TEST_F("require that blocked jobs are not executed", MaintenanceControllerFixture)
{
    auto job = std::make_unique<MySimpleJob>(200ms, 200ms, 0);
    MySimpleJob &myJob = *job;
    myJob.block();
    f._mc.registerJobInMasterThread(std::move(job));
    f._injectDefaultJobs = false;
    f.startMaintenance();
    std::this_thread::sleep_for(2s);
    EXPECT_EQUAL(0u, myJob._runCnt);
}

TEST_F("require that maintenance controller state list jobs", MaintenanceControllerFixture)
{
    {
        IMaintenanceJob::UP job1(new MySimpleJob(TIMEOUT_SEC * 2, TIMEOUT_SEC * 2, 0));
        IMaintenanceJob::UP job2(new MyLongRunningJob(200ms, 200ms));
        auto &longRunningJob = dynamic_cast<MyLongRunningJob &>(*job2);
        f._mc.registerJobInMasterThread(std::move(job1));
        f._mc.registerJobInMasterThread(std::move(job2));
        f._injectDefaultJobs = false;
        f.startMaintenance();
        longRunningJob._firstRun.await(TIMEOUT_MS);
    }

    MaintenanceControllerExplorer explorer(f._mc.getJobList());
    Slime state;
    SlimeInserter inserter(state);
    explorer.get_state(inserter, true);

    Inspector &runningJobs = state.get()["runningJobs"];
    EXPECT_EQUAL(1u, runningJobs.children());
    EXPECT_EQUAL("long_running_job", runningJobs[0]["name"].asString().make_string());

    Inspector &allJobs = state.get()["allJobs"];
    EXPECT_EQUAL(2u, allJobs.children());
    EXPECT_EQUAL("my_job", allJobs[0]["name"].asString().make_string());
    EXPECT_EQUAL("long_running_job", allJobs[1]["name"].asString().make_string());
}

TEST("Verify FrozenBucketsMap interface") {
    FrozenBucketsMap m;
    BucketId a(8, 6);
    {
        auto guard = m.acquireExclusiveBucket(a);
        EXPECT_TRUE(bool(guard));
        EXPECT_EQUAL(a, guard->getBucket());
    }
    m.freezeBucket(a);
    EXPECT_FALSE(m.thawBucket(a));
    m.freezeBucket(a);
    {
        auto guard = m.acquireExclusiveBucket(a);
        EXPECT_FALSE(bool(guard));
    }
    EXPECT_TRUE(m.thawBucket(a));
    m.freezeBucket(a);
    m.freezeBucket(a);
    m.freezeBucket(a);
    {
        auto guard = m.acquireExclusiveBucket(a);
        EXPECT_FALSE(bool(guard));
    }
    EXPECT_FALSE(m.thawBucket(a));
    EXPECT_FALSE(m.thawBucket(a));
    EXPECT_TRUE(m.thawBucket(a));
    {
        auto guard = m.acquireExclusiveBucket(a);
        EXPECT_TRUE(bool(guard));
        EXPECT_EQUAL(a, guard->getBucket());
    }
}

const MaintenanceJobRunner *
findJob(const MaintenanceController::JobList &jobs, const vespalib::string &jobName)
{
    auto itr = std::find_if(jobs.begin(), jobs.end(),
                            [&](const auto &job){ return job->getJob().getName() == jobName; });
    if (itr != jobs.end()) {
        return itr->get();
    }
    return nullptr;
}

bool
containsJob(const MaintenanceController::JobList &jobs, const vespalib::string &jobName)
{
    return findJob(jobs, jobName) != nullptr;
}

bool
containsJobAndExecutedBy(const MaintenanceController::JobList &jobs, const vespalib::string &jobName,
                         const vespalib::Executor & executor)
{
    const auto *job = findJob(jobs, jobName);
    return (job != nullptr) && (&job->getExecutor() == &executor);
}

TEST_F("require that lid space compaction jobs can be disabled", MaintenanceControllerFixture)
{
    f._lscHandlers.push_back(std::make_unique<MockLidSpaceCompactionHandler>("my_handler"));
    f.forwardMaintenanceConfig();
    {
        auto jobs = f._mc.getJobList();
        EXPECT_EQUAL(6u, jobs.size());
        EXPECT_TRUE(containsJob(jobs, "lid_space_compaction.my_handler"));
    }
    f.setLidSpaceCompactionConfig(DocumentDBLidSpaceCompactionConfig::createDisabled());
    {
        auto jobs = f._mc.getJobList();
        EXPECT_EQUAL(5u, jobs.size());
        EXPECT_FALSE(containsJob(jobs, "lid_space_compaction.my_handler"));
    }
}

TEST_F("require that maintenance jobs are run by correct executor", MaintenanceControllerFixture)
{
    f.injectMaintenanceJobs();
    auto jobs = f._mc.getJobList();
    EXPECT_EQUAL(5u, jobs.size());
    EXPECT_TRUE(containsJobAndExecutedBy(jobs, "heart_beat", f._threadService));
    EXPECT_TRUE(containsJobAndExecutedBy(jobs, "prune_session_cache", f._genericExecutor));
    EXPECT_TRUE(containsJobAndExecutedBy(jobs, "prune_removed_documents.searchdocument", f._threadService));
    EXPECT_TRUE(containsJobAndExecutedBy(jobs, "move_buckets.searchdocument", f._threadService));
    EXPECT_TRUE(containsJobAndExecutedBy(jobs, "sample_attribute_usage.searchdocument", f._threadService));
}

void
assertPruneRemovedDocumentsConfig(vespalib::duration expDelay, vespalib::duration expInterval, vespalib::duration interval, MaintenanceControllerFixture &f)
{
    f.setPruneConfig(DocumentDBPruneRemovedDocumentsConfig(interval, 1000s));
    const auto *job = findJob(f._mc.getJobList(), "prune_removed_documents.searchdocument");
    EXPECT_EQUAL(expDelay, job->getJob().getDelay());
    EXPECT_EQUAL(expInterval, job->getJob().getInterval());
}

TEST_F("require that delay for prune removed documents is set based on interval and is max 300 secs", MaintenanceControllerFixture)
{
    assertPruneRemovedDocumentsConfig(300s, 301s, 301s, f);
    assertPruneRemovedDocumentsConfig(299s, 299s, 299s, f);
}

TEST_MAIN()
{
    TEST_RUN_ALL();
}
