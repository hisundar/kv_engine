/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include "magma-kvstore.h"
#include "magma-wrapper.h"

#include "ep_time.h"

#include "magma-kvstore_config.h"
#include "kvstore_priv.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <limits>

#include "vbucket.h"

namespace magmakv {
// MetaData is used to serialize and de-serialize metadata respectively when
// writing a Document mutation request to Magma and when reading a Document
// from Magma.
class MetaData {
public:
    MetaData()
        : deleted(0),
          version(0),
          datatype(0),
          flags(0),
          valueSize(0),
          exptime(0),
          cas(0),
          revSeqno(0),
          bySeqno(0){};
    MetaData(bool deleted,
             uint8_t version,
             uint8_t datatype,
             uint32_t flags,
             uint32_t valueSize,
             time_t exptime,
             uint64_t cas,
             uint64_t revSeqno,
             int64_t bySeqno)
        : deleted(deleted),
          version(version),
          datatype(datatype),
          flags(flags),
          valueSize(valueSize),
          exptime(exptime),
          cas(cas),
          revSeqno(revSeqno),
          bySeqno(bySeqno){};

// The `#pragma pack(1)` directive and the order of members are to keep
// the size of MetaData as small as possible and uniform across different
// platforms.
#pragma pack(1)
    uint8_t deleted : 1;
    uint8_t version : 7;
    uint8_t datatype;
    uint32_t flags;
    uint32_t valueSize;
    time_t exptime;
    uint64_t cas;
    uint64_t revSeqno;
    int64_t bySeqno;
#pragma pack()
};
} // namespace magmakv

/**
 * Class representing a document to be persisted in Magma.
 */
class MagmaRequest : public IORequest {
public:
    /**
     * Constructor
     *
     * @param item Item instance to be persisted
     * @param callback Persistence Callback
     * @param del Flag indicating if it is an item deletion or not
     */
    MagmaRequest(const Item& item, MutationRequestCallback& callback)
        : IORequest(item.getVBucketId(),
                    callback,
                    item.isDeleted(),
                    item.getKey()),
          docBody(item.getValue()),
	      updatedExistingItem(false) {
        docMeta = magmakv::MetaData(
                item.isDeleted(),
                0,
                item.getDataType(),
                item.getFlags(),
                item.getNBytes(),
                item.isDeleted() ? ep_real_time() : item.getExptime(),
                item.getCas(),
                item.getRevSeqno(),
                item.getBySeqno());
    }

    const magmakv::MetaData& getDocMeta() {
        return docMeta;
    }

    const int64_t getBySeqno() {
        return docMeta.bySeqno;
    }

    const size_t getKeyLen() {
        return getKey().size();
    }

    const char *getKeyData() {
        return getKey().c_str();
    }
    const size_t getBodySize() {
        return docBody ? docBody->valueSize() : 0;
    }

    const void *getBodyData() {
        return docBody ? docBody->getData() : nullptr;
    }

	const bool wasCreate() { return !updatedExistingItem; }
	void markAsUpdated() { updatedExistingItem = true; }

private:
    magmakv::MetaData docMeta;
    value_t docBody;
	bool updatedExistingItem;
};

// using PlsmPtr = std::unique_ptr<magmakv::DB>;

class KVMagma {
public:
    KVMagma(const uint16_t vbid, const std::string path) :
        vbid(vbid) {
        magmaHandleId = open_magma(path.c_str(), vbid);
        if (magmaHandleId < 0) {
            fprintf(stderr, "FATAL: Unable to open magma %s, vb %d\n",
                    path.c_str(), vbid);
            throw std::logic_error("MagmaKVStore::openDB: can't open[" +
                    std::to_string(vbid) + "] in " + path.c_str());
        }
    }

    ~KVMagma() {
        uint64_t persistedSeqno;
        close_magma(vbid, magmaHandleId, &persistedSeqno);
    }

    int SetOrDel(const std::unique_ptr<MagmaRequest>& req) {
        if (req->isDelete()) {
            //fprintf(stderr, "Deleting a key %s, len %zu from magma",
             //       req->getKeyData(), req->getKeyLen());
            return delete_kv(Magma_KVengine, vbid, magmaHandleId,
                    req->getKeyData(), req->getKeyLen());
        }
       // fprintf(stderr, "Inserting a key %s, len %zu into magma",
        //        req->getKeyData(), req->getKeyLen());

        /* TODO: Send in the slices of magma meta & value to avoid memcpy */
        std::memcpy(&big_bad_buf[0], &req->getDocMeta(),
                    sizeof(magmakv::MetaData));
		auto valSz = req->getBodySize();
		if (valSz > 3000) {
			fprintf(stderr, "FATAL-TOO-BIG-VALUE: val size = %zu\n",
					req->getBodySize());
			valSz = 3000;
		}
        if (req->getBySeqno() == 0) {
			fprintf(stderr, "FATAL-ZERO-SEQNUM-IN-INSERT: val size = %zu\n",
					req->getBodySize());
            throw std::logic_error("ZERO SEQNUM SHOULD NOT EXIST!!");
        }
        std::memcpy(&big_bad_buf[sizeof(magmakv::MetaData)], req->getBodyData(),
                    valSz);
        int ret = insert_kv(Magma_KVengine, vbid, magmaHandleId,
                req->getKeyData(), req->getKeyLen(),
                &big_bad_buf[0], valSz + sizeof(magmakv::MetaData),
                req->getBySeqno());
		if (ret < 0) {
			return ret;
		}
		if (ret == 1) { // Item previously existing in magma
			req->markAsUpdated();
		}
		return 0;
    }

    int Get(const StoredDocKey &key, void **value, int *valueLen) {
		*value = &big_bad_buf;
        *valueLen = sizeof(big_bad_buf);
        int ret = lookup_kv(Magma_KVengine, vbid, magmaHandleId,
                key.data(), key.size(), value, valueLen);
		if (ret) {
			fprintf(stderr, "FATAL-MAGMA-LOOKUP-ERROR: %d\n", ret);
		}
		return ret;
    }

    uint16_t vbid;
    int magmaHandleId;
    char big_bad_buf[3072];
};

static std::mutex initGuard;
static bool magmaInited;

MagmaKVStore::MagmaKVStore(MagmaKVStoreConfig& configuration)
    : KVStore(configuration),
      vbDB(configuration.getMaxVBuckets()),
      in_transaction(false),
      magmaPath(configuration.getDBName()+ "/magma"),
      scanCounter(0),
      logger(configuration.getLogger()) {

    {
        LockHolder lh(initGuard);
        if (!magmaInited) {
			uint64_t memQuota = uint64_t(configuration.getMagmaMemQuota());
			memQuota *= (1024 * 1024); // Input is in MB, convert to bytes
			bool directIo = configuration.isMagmaEnableDirectio();
			bool kvSeparate = configuration.isMagmaKvSeparation();
			int lssCleanAtFrag = configuration.getMagmaLssCleanThreshold();
			int lssCleanMax = configuration.getMagmaLssCleanMax();
			int deltaChainLen = configuration.getMagmaDeltaChainLen();
			int basePageLen = configuration.getMagmaBasePageItems();
			int lssNumSegs = configuration.getMagmaLssNumSegments();
			int syncAt = configuration.getMagmaSyncAt();
			bool upsert = configuration.isMagmaEnableUpsert();

            init_magma(memQuota,
					directIo,
					kvSeparate,
					lssCleanAtFrag,
					lssCleanMax,
					deltaChainLen,
					basePageLen,
					lssNumSegs,
					syncAt,
					upsert);
            magmaInited = true;
            fprintf(stderr, "Initialized magma kvstore..\n");
            fprintf(stderr, "MemQuota = %zu\n", memQuota);
            fprintf(stderr, "DirectIO (%s)\n", directIo ? "yes" : "no");
            fprintf(stderr, "KV Separation (%s)\n", kvSeparate ? "yes" : "no");
            fprintf(stderr, "LSS clean at %d\n", lssCleanAtFrag);
            fprintf(stderr, "LSS throttle at %d\n", lssCleanMax);
            fprintf(stderr, "Delta Chain Len %d\n", deltaChainLen);
            fprintf(stderr, "Base Page Len %d\n", basePageLen);
            fprintf(stderr, "LSS Num Segments %d\n", lssNumSegs);
            fprintf(stderr, "Sync at %d milliseconds\n", syncAt);
            fprintf(stderr, "Upsert (%s)\n", upsert ? "yes" : "no");
        }
    }
    cachedVBStates.resize(configuration.getMaxVBuckets());

    createDataDir(configuration.getDBName());

    // Read persisted VBs state
    auto vbids = discoverVBuckets();
    for (auto vbid : vbids) {
        KVMagma db(vbid, magmaPath);
        //readVBState(db);
        // Update stats
        ++st.numLoadedVb;
    }
}

MagmaKVStore::~MagmaKVStore() {
    in_transaction = false;
}

std::string MagmaKVStore::getVBDBSubdir(uint16_t vbid) {
    return configuration.getDBName() + "/magma." + std::to_string(vbid);
}

std::vector<uint16_t> MagmaKVStore::discoverVBuckets() {
    std::vector<uint16_t> vbids;
    auto vbDirs =
            cb::io::findFilesContaining(configuration.getDBName(), "magma.");
    for (auto& dir : vbDirs) {
        size_t lastDotIndex = dir.rfind(".");
        size_t vbidLength = dir.size() - lastDotIndex - 1;
        std::string vbidStr = dir.substr(lastDotIndex + 1, vbidLength);
        uint16_t vbid = atoi(vbidStr.c_str());
        // Take in account only VBuckets managed by this Shard
        if ((vbid % configuration.getMaxShards()) ==
            configuration.getShardId()) {
            vbids.push_back(vbid);
        }
    }
    return vbids;
}

bool MagmaKVStore::begin(std::unique_ptr<TransactionContext> txCtx) {
    in_transaction = true;
    transactionCtx = std::move(txCtx);
    return in_transaction;
}

bool MagmaKVStore::commit(const Item* collectionsManifest) {
    // This behaviour is to replicate the one in Couchstore.
    // If `commit` is called when not in transaction, just return true.
    if (!in_transaction) {
        return true;
    }

    if (pendingReqs.size() == 0) {
        in_transaction = false;
        return true;
    }

    // Swap `pendingReqs` with the temporary `commitBatch` so that we can
    // shorten the scope of the lock.
    std::vector<std::unique_ptr<MagmaRequest>> commitBatch;
    {
        std::lock_guard<std::mutex> lock(writeLock);
        std::swap(pendingReqs, commitBatch);
    }

    bool success = true;
    auto vbid = commitBatch[0]->getVBucketId();

    // Flush all documents to disk
    auto status = saveDocs(vbid, collectionsManifest, commitBatch);
    if (status) {
        logger.log(EXTENSION_LOG_WARNING,
                   "MagmaKVStore::commit: saveDocs error:%d, "
                   "vb:%" PRIu16,
                   status,
                   vbid);
        success = false;
    }

    commitCallback(status, commitBatch);

    // This behaviour is to replicate the one in Couchstore.
    // Set `in_transanction = false` only if `commit` is successful.
    if (success) {
        in_transaction = false;
        transactionCtx.reset();
    }

    return success;
}

void MagmaKVStore::commitCallback(
        int status,
        const std::vector<std::unique_ptr<MagmaRequest>>& commitBatch) {
    for (const auto& req : commitBatch) {
        if (!status) {
            ++st.numSetFailure;
        } else {
            st.writeTimeHisto.add(req->getDelta() / 1000);
            st.writeSizeHisto.add(req->getKeyLen() + req->getBodySize());
        }
        // TODO: Should set `mr.second` to true or false depending on if
        // this is an insertion (true) or an update of an existing item
        // (false). However, to achieve this we would need to perform a lookuup
        // to RocksDB which is costly. For now just assume that the item
        // did not exist.
        mutation_result mr = std::make_pair(1, req->wasCreate());
		req->getSetCallback()->callback(*transactionCtx, mr);
    }
}

void MagmaKVStore::rollback() {
    if (in_transaction) {
        in_transaction = false;
        transactionCtx.reset();
    }
}

StorageProperties MagmaKVStore::getStorageProperties(void) {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::No,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::Yes);
    return rv;
}

std::vector<vbucket_state*> MagmaKVStore::listPersistedVbuckets() {
    std::vector<vbucket_state*> result;
    for (const auto& vb : cachedVBStates) {
        result.emplace_back(vb.get());
    }
    return result;
}

void MagmaKVStore::set(const Item& item,
                         Callback<TransactionContext, mutation_result>& cb) {
    if (!in_transaction) {
        throw std::logic_error(
                "MagmaKVStore::set: in_transaction must be true to perform a "
                "set operation.");
    }
    MutationRequestCallback callback;
    callback.setCb = &cb;
    pendingReqs.push_back(std::make_unique<MagmaRequest>(item, callback));
}

GetValue MagmaKVStore::get(const StoredDocKey& key, uint16_t vb, bool fetchDelete) {
    return getWithHeader(nullptr, key, vb, GetMetaOnly::No, fetchDelete);
}

GetValue MagmaKVStore::getWithHeader(void* dbHandle,
                                       const StoredDocKey& key,
                                       uint16_t vb,
                                       GetMetaOnly getMetaOnly,
                                       bool fetchDelete) {
    void *value;
    int valueLen;
    KVMagma db(vb, magmaPath);
    int status = db.Get(key, &value, &valueLen);
    if (status < 0) {
        logger.log(EXTENSION_LOG_WARNING,
                "MagmaKVStore::getWithHeader: magma::DB::Lookup error:%d, "
                "vb:%" PRIu16,
                status,
                vb);
    }
    std::string valStr(reinterpret_cast<char *>(value), valueLen);
    return makeGetValue(vb, key, valStr, getMetaOnly);
}

void MagmaKVStore::getMulti(uint16_t vb, vb_bgfetch_queue_t& itms) {
    KVMagma db(vb, magmaPath);
    for (auto& it : itms) {
		auto &key = it.first;
		void *value;
		int valueLen;
		int status = db.Get(key, &value, &valueLen);
		if (status < 0) {
			logger.log(EXTENSION_LOG_WARNING,
					"MagmaKVStore::getMulti: magma::DB::Lookup error:%d, "
					"vb:%" PRIu16,
					status,
					vb);
			for (auto &fetch : it.second.bgfetched_list) {
				fetch->value->setStatus(ENGINE_KEY_ENOENT);
			}
			continue;
		}
		std::string valStr(reinterpret_cast<char *>(value), valueLen);
		it.second.value = makeGetValue(vb, key, valStr, it.second.isMetaOnly);
		GetValue *rv = &it.second.value;
		for (auto &fetch : it.second.bgfetched_list) {
			fetch->value = rv;
		}
    }
}

void MagmaKVStore::reset(uint16_t vbucketId) {
    // TODO Plsm:  Implement.
}

void MagmaKVStore::del(const Item& item,
                         Callback<TransactionContext, int>& cb) {
    if (!in_transaction) {
        throw std::logic_error(
                "MagmaKVStore::del: in_transaction must be true to perform a "
                "delete operation.");
    }
    // TODO: Deleted items remain as tombstones, but are not yet expired,
    // they will accumuate forever.
    MutationRequestCallback callback;
    callback.delCb = &cb;
    pendingReqs.push_back(std::make_unique<MagmaRequest>(item, callback));
}

void MagmaKVStore::delVBucket(uint16_t vbid, uint64_t vb_version) {
    std::lock_guard<std::mutex> lg(writeLock);
    // TODO: check if needs lock on `openDBMutex`. We should not need (e.g.,
    // there was no synchonization between this and `commit`), but we could
    // have an error if we destroy `vbDB[vbid]` while the same DB is used
    // somewhere else. Also, from Magma docs:
    //     "Calling DestroyDB() on a live DB is an undefined behavior."
    vbDB[vbid].reset();
    // Just destroy the DB in the sub-folder for vbid
    auto dbname = getVBDBSubdir(vbid);
    // DESTROY DB...
}

bool MagmaKVStore::snapshotVBucket(uint16_t vbucketId,
                                     const vbucket_state& vbstate,
                                     VBStatePersist options) {
    // TODO Plsm: Refactor out behaviour common to this and CouchKVStore
    auto start = ProcessClock::now();

    if (updateCachedVBState(vbucketId, vbstate) &&
        (options == VBStatePersist::VBSTATE_PERSIST_WITHOUT_COMMIT ||
         options == VBStatePersist::VBSTATE_PERSIST_WITH_COMMIT)) {
        int handleId = open_magma(magmaPath.c_str(), vbucketId);
        uint64_t persistedSeqno;
        close_magma(vbucketId, handleId, &persistedSeqno);
        /*
        auto& db = openDB(vbucketId);
        if (!saveVBState(db, vbstate).ok()) {
            logger.log(EXTENSION_LOG_WARNING,
                       "MagmaKVStore::snapshotVBucket: saveVBState failed "
                       "state:%s, vb:%" PRIu16,
                       VBucket::toString(vbstate.state),
                       vbucketId);
            return false;
        }
        */
    }

    LOG(EXTENSION_LOG_DEBUG,
        "MagmaKVStore::snapshotVBucket: Snapshotted vbucket:%" PRIu16
        " state:%s",
        vbucketId,
        vbstate.toJSON().c_str());

    st.snapshotHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - start));

    return true;
}

bool MagmaKVStore::snapshotStats(const std::map<std::string, std::string>&) {
    // TODO Plsm:  Implement
    return true;
}

void MagmaKVStore::destroyInvalidVBuckets(bool) {
    // TODO Plsm:  implement
}

size_t MagmaKVStore::getNumShards() {
    return configuration.getMaxShards();
}

std::unique_ptr<Item> MagmaKVStore::makeItem(uint16_t vb,
                                              const DocKey& key,
                                              const std::string& value,
                                              GetMetaOnly getMetaOnly) {
    const char* data = value.c_str();

    magmakv::MetaData meta;
    std::memcpy(&meta, data, sizeof(meta));
    data += sizeof(meta);

    bool includeValue = getMetaOnly == GetMetaOnly::No && meta.valueSize;

    auto item = std::make_unique<Item>(key,
                                       meta.flags,
                                       meta.exptime,
                                       includeValue ? data : nullptr,
                                       includeValue ? meta.valueSize : 0,
                                       meta.datatype,
                                       meta.cas,
                                       meta.bySeqno,
                                       vb,
                                       meta.revSeqno);

    if (meta.deleted) {
        item->setDeleted();
    }

    return item;
}

GetValue MagmaKVStore::makeGetValue(uint16_t vb,
                                      const DocKey& key,
                                      const std::string & value,
                                      GetMetaOnly getMetaOnly) {
    return GetValue(
            makeItem(vb, key, value, getMetaOnly), ENGINE_SUCCESS, -1, 0);
}

void MagmaKVStore::readVBState(const KVMagma& db) {
    // Largely copied from CouchKVStore
    // TODO Plsm: refactor out sections common to CouchKVStore
    vbucket_state_t state = vbucket_state_dead;
    uint64_t checkpointId = 0;
    uint64_t maxDeletedSeqno = 0;
    int64_t highSeqno = readHighSeqnoFromDisk(db);
    std::string failovers;
    uint64_t purgeSeqno = 0;
    uint64_t lastSnapStart = 0;
    uint64_t lastSnapEnd = 0;
    uint64_t maxCas = 0;
    int64_t hlcCasEpochSeqno = HlcCasSeqnoUninitialised;
    bool mightContainXattrs = false;

    auto key = getVbstateKey();
    std::string vbstate;
    auto vbid = db.vbid;
    cachedVBStates[vbid] = std::make_unique<vbucket_state>(state,
                                                           checkpointId,
                                                           maxDeletedSeqno,
                                                           highSeqno,
                                                           purgeSeqno,
                                                           lastSnapStart,
                                                           lastSnapEnd,
                                                           maxCas,
                                                           hlcCasEpochSeqno,
                                                           mightContainXattrs,
                                                           failovers);
}

int MagmaKVStore::saveDocs(
        uint16_t vbid,
        const Item* collectionsManifest,
        const std::vector<std::unique_ptr<MagmaRequest>>& commitBatch) {
    auto reqsSize = commitBatch.size();
    if (reqsSize == 0) {
        st.docsCommitted = 0;
        return 0;
    }

    auto& vbstate = cachedVBStates[vbid];
    if (vbstate == nullptr) {
        throw std::logic_error("MagmaKVStore::saveDocs: cachedVBStates[" +
                               std::to_string(vbid) + "] is NULL");
    }

    int64_t lastSeqno = 0;
    int status = 0;

    auto begin = ProcessClock::now();
    {
        KVMagma db(vbid, magmaPath);

        for (const auto& request : commitBatch) {
            status = db.SetOrDel(request);
            if (status < 0) {
                logger.log(EXTENSION_LOG_WARNING,
                        "MagmaKVStore::saveDocs: magma::DB::Insert error:%d, "
                        "vb:%" PRIu16,
                        status,
                        vbid);
            }
            if (request->getBySeqno() > lastSeqno) {
                lastSeqno = request->getBySeqno();
            }
        }
    }

    st.commitHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            ProcessClock::now() - begin));
    if (status) {
        logger.log(EXTENSION_LOG_WARNING,
                   "MagmaKVStore::saveDocs: magma::DB::Write error:%d, "
                   "vb:%" PRIu16,
                   status,
                   vbid);
        return status;
    }

    vbstate->highSeqno = lastSeqno;

    return status;
}

int64_t MagmaKVStore::readHighSeqnoFromDisk(const KVMagma& db) {
    return 0;
}

std::string MagmaKVStore::getVbstateKey() {
    return "vbstate";
}

ScanContext* MagmaKVStore::initScanContext(
        std::shared_ptr<StatusCallback<GetValue>> cb,
        std::shared_ptr<StatusCallback<CacheLookup>> cl,
        uint16_t vbid,
        uint64_t startSeqno,
        DocumentFilter options,
        ValueFilter valOptions) {
    size_t scanId = scanCounter++;

    // As we cannot efficiently determine how many documents this scan will
    // find, we approximate this value with the seqno difference + 1
    // as scan is supposed to be inclusive at both ends,
    // seqnos 2 to 4 covers 3 docs not 4 - 2 = 2

    uint64_t endSeqno = cachedVBStates[vbid]->highSeqno;
    return new ScanContext(cb,
                           cl,
                           vbid,
                           scanId,
                           startSeqno,
                           endSeqno,
                           0, /*TODO MAGMA: pass the read purge-seqno */
                           options,
                           valOptions,
                           /* documentCount */ endSeqno - startSeqno + 1,
                           configuration);
}

scan_error_t MagmaKVStore::scan(ScanContext* ctx) {
	if (!ctx) {
        return scan_failed;
    }

    if (ctx->lastReadSeqno == ctx->maxSeqno) {
        return scan_success;
    }

    auto startSeqno = ctx->startSeqno;
    if (ctx->lastReadSeqno != 0) {
        startSeqno = ctx->lastReadSeqno + 1;
    }

    GetMetaOnly isMetaOnly = ctx->valFilter == ValueFilter::KEYS_ONLY
                                     ? GetMetaOnly::Yes
                                     : GetMetaOnly::No;

	logger.log(EXTENSION_LOG_WARNING,
			"MagmaKVStore::scan from start seqno %zu to %zu on vb %d",
			startSeqno, ctx->maxSeqno, ctx->vbid);

	int bfillHandle = open_backfill_query(ctx->vbid, startSeqno);

    if (bfillHandle < 0) {
		char errbf[256];
		sprintf(errbf, "MagmaKVStore::scan: magma backfill query fail! err=%d vbid=%d, startseqno=%zu", bfillHandle, ctx->vbid, startSeqno);
        throw std::logic_error(errbf);
    }

	char keyBuf[200]; // TODO: Find a way to have Magma allocate memory
	void *Key = &keyBuf;
	int keyLen = sizeof(keyBuf);
	char valueBuf[3072]; // TODO: Find a way to have Magma to allocate memory
	void *value = &valueBuf;
	int valueLen = sizeof(valueBuf);
	uint64_t seqNo;

	while (true) {
		keyLen = sizeof(keyBuf); // reset back for every query call
		valueLen = sizeof(valueBuf);
	    int err = next_backfill_query(ctx->vbid, bfillHandle, &Key, &keyLen,
				&value, &valueLen, &seqNo);
		if (err) {
		    if (err == ErrBackfillQueryEOF) {
			logger.log(EXTENSION_LOG_WARNING,
					"BACKFILL complete for vb %d: max seqno %zu\n",
					ctx->vbid, ctx->maxSeqno);
		        break;
		    }
			fprintf(stderr, "FATAL-MAGMA-BACKFILL-ERROR: %d\n", err);
			throw std::logic_error(
                "MagmaKVStore::scan: magma backfill query next fail!");
		}
        if (int64_t(seqNo) > ctx->maxSeqno) { // don't return sequence numbers out of snapshot
            continue;
        }
		DocKey key(reinterpret_cast<const uint8_t*>(Key), keyLen,
				DocNamespace::DefaultCollection);

		std::string valStr(reinterpret_cast<char *>(value), valueLen);
        std::unique_ptr<Item> itm =
                makeItem(ctx->vbid, key, valStr, isMetaOnly);
		bool includeDeletes =
                (ctx->docFilter == DocumentFilter::NO_DELETES) ? false : true;
        bool onlyKeys =
                (ctx->valFilter == ValueFilter::KEYS_ONLY) ? true : false;

		if (!includeDeletes && itm->isDeleted()) {
            continue;
        }
        int64_t byseqno = seqNo;
        CacheLookup lookup(key, byseqno, ctx->vbid,
				ctx->collectionsContext.getSeparator());
        ctx->lookup->callback(lookup);

        int status = ctx->lookup->getStatus();

        if (status == ENGINE_KEY_EEXISTS) {
            ctx->lastReadSeqno = byseqno;
            continue;
        } else if (status == ENGINE_ENOMEM) {
			logger.log(EXTENSION_LOG_WARNING,
					"BACKFILL scan-again: cache lookup ENOMEM: %zu %zu %d\n",
					startSeqno, ctx->maxSeqno, ctx->vbid);
            return scan_again;
        }

        GetValue rv(std::move(itm), ENGINE_SUCCESS, -1, onlyKeys);
        ctx->callback->callback(rv);
        status = ctx->callback->getStatus();

        if (status == ENGINE_ENOMEM) {
			logger.log(EXTENSION_LOG_WARNING,
					"BACKFILL scan-again: value callback ENOMEM: %zu %zu %d\n",
					startSeqno, ctx->maxSeqno, ctx->vbid);
            return scan_again;
        }

        ctx->lastReadSeqno = byseqno;
	}
	close_backfill_query(ctx->vbid, bfillHandle);

    return scan_success;
}

void MagmaKVStore::destroyScanContext(ScanContext* ctx) {
    // TODO Plsm: Might be nice to have the snapshot in the ctx and
    // release it on destruction
}