// wiredtiger_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include <wiredtiger.h>

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

//#define RS_ITERATOR_TRACE(x) log() << "WTRS::Iterator " << x
#define RS_ITERATOR_TRACE(x)

namespace mongo {
namespace {
    bool shouldUseOplogHack(OperationContext* opCtx, const std::string& uri) {
        WiredTigerCursor curwrap("metadata:", WiredTigerSession::kMetadataCursorId, opCtx);
        WT_CURSOR* c = curwrap.get();
        c->set_key(c, uri.c_str());
        int ret = c->search(c);
        if (ret == WT_NOTFOUND)
            return false;
        invariantWTOK(ret);

        const char* config = NULL;
        c->get_value(c, &config);
        invariant(config);

        WiredTigerConfigParser topParser(config);
        WT_CONFIG_ITEM metadata;
        if (topParser.get("app_metadata", &metadata) != 0)
            return false;

        if (metadata.len == 0)
            return false;

        WiredTigerConfigParser parser(metadata);
        WT_CONFIG_ITEM keyItem;
        WT_CONFIG_ITEM value;
        while (parser.next(&keyItem, &value) == 0) {
            const StringData key(keyItem.str, keyItem.len);
            if (key == "oplogKeyExtractionVersion") {
                if (value.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM &&  value.val == 1)
                    return true;
            }

            // This prevents downgrades with unsupported metadata settings.
            severe() << "Unrecognized WiredTiger metadata setting: " << key << '=' << value.str;
            fassertFailedNoTrace(28548);
        }

        return false;
    }

    class CappedInsertChange : public RecoveryUnit::Change {
    public:
        CappedInsertChange( WiredTigerRecordStore* rs, const RecordId& loc )
            : _rs( rs ), _loc( loc ) {
        }

        virtual void commit() {
            _rs->dealtWithCappedLoc( _loc );
        }

        virtual void rollback() {
            _rs->dealtWithCappedLoc( _loc );
        }

    private:
        WiredTigerRecordStore* _rs;
        RecordId _loc;
    };

} // namespace

    const std::string kWiredTigerEngineName = "wiredTiger";

    StatusWith<std::string> WiredTigerRecordStore::generateCreateString(const StringData& ns,
                                                                        const CollectionOptions& options,
                                                                        const StringData& extraStrings) {
        // Separate out a prefix and suffix in the default string. User configuration will
        // override values in the prefix, but not values in the suffix.
        str::stream ss;
        ss << "type=file,";
        ss << "memory_page_max=100m,";
        ss << "block_compressor=snappy,";

        ss << extraStrings << ",";

        // Validate configuration object.
        // Warn about unrecognized fields that may be introduced in newer versions of this
        // storage engine instead of raising an error.
        // Ensure that 'configString' field is a string. Warn if this is not the case.
        BSONForEach(elem, options.storageEngine.getObjectField(kWiredTigerEngineName)) {
            if (elem.fieldNameStringData() == "configString") {
                if (elem.type() != String) {
                    return StatusWith<std::string>(ErrorCodes::TypeMismatch, str::stream()
                        << "storageEngine.wiredTiger.configString must be a string. "
                        << "Not adding 'configString' value "
                        << elem << " to collection configuration");
                    continue;
                }
                ss << elem.valueStringData() << ",";
            }
            else {
                // Return error on first unrecognized field.
                return StatusWith<std::string>(ErrorCodes::InvalidOptions, str::stream()
                    << '\'' << elem.fieldNameStringData() << '\''
                    << " is not a supported option in storageEngine.wiredTiger");
            }
        }

        if ( NamespaceString::oplog(ns) ) {
            // force file for oplog
            ss << "type=file,";
            ss << "app_metadata=(oplogKeyExtractionVersion=1),";
            // Tune down to 10m.  See SERVER-16247
            ss << "memory_page_max=10m,";
        }
        else {
            // Force this to be empty since users shouldn't be allowed to change it.
            ss << "app_metadata=(),";
        }

        ss << "key_format=q,value_format=u";
        return StatusWith<std::string>(ss);
    }

    WiredTigerRecordStore::WiredTigerRecordStore(OperationContext* ctx,
                                                 const StringData& ns,
                                                 const StringData& uri,
                                                 bool isCapped,
                                                 int64_t cappedMaxSize,
                                                 int64_t cappedMaxDocs,
                                                 CappedDocumentDeleteCallback* cappedDeleteCallback,
                                                 WiredTigerSizeStorer* sizeStorer)
            : RecordStore( ns ),
              _uri( uri.toString() ),
              _instanceId( WiredTigerSession::genCursorId() ),
              _isCapped( isCapped ),
              _isOplog( NamespaceString::oplog( ns ) ),
              _cappedMaxSize( cappedMaxSize ),
              _cappedMaxDocs( cappedMaxDocs ),
              _cappedDeleteCallback( cappedDeleteCallback ),
              _useOplogHack(shouldUseOplogHack(ctx, _uri)),
              _sizeStorer( sizeStorer ),
              _sizeStorerCounter(0)
    {

        if (_isCapped) {
            invariant(_cappedMaxSize > 0);
            invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
        }
        else {
            invariant(_cappedMaxSize == -1);
            invariant(_cappedMaxDocs == -1);
        }

        // Find the largest RecordId currently in use and estimate the number of records.
        scoped_ptr<RecordIterator> iterator( getIterator( ctx, RecordId(),
                                                          CollectionScanParams::BACKWARD ) );
        if ( iterator->isEOF() ) {
            _dataSize.store(0);
            _numRecords.store(0);
            // Need to start at 1 so we are always higher than RecordId::min()
            _nextIdNum.store( 1 );
            if ( sizeStorer )
                _sizeStorer->onCreate( this, 0, 0 );
        }
        else {
            RecordId maxLoc = iterator->curr();
            uint64_t max = _makeKey( maxLoc );
            _oplog_highestSeen = maxLoc;
            _nextIdNum.store( 1 + max );

            if ( _sizeStorer ) {
                long long numRecords;
                long long dataSize;
                _sizeStorer->load( uri, &numRecords, &dataSize );
                _numRecords.store( numRecords );
                _dataSize.store( dataSize );
                _sizeStorer->onCreate( this, numRecords, dataSize );
            }

            if ( _sizeStorer == NULL || _numRecords.load() < 10000 ) {
                LOG(1) << "doing scan of collection " << ns << " to get info";

                _numRecords.store(0);
                _dataSize.store(0);

                while( !iterator->isEOF() ) {
                    RecordId loc = iterator->getNext();
                    RecordData data = iterator->dataFor( loc );
                    _numRecords.fetchAndAdd(1);
                    _dataSize.fetchAndAdd(data.size());
                }

                if ( _sizeStorer ) {
                    _sizeStorer->store( _uri, _numRecords.load(), _dataSize.load() );
                }
            }

        }

    }

    WiredTigerRecordStore::~WiredTigerRecordStore() {
        LOG(1) << "~WiredTigerRecordStore for: " << ns();
        if ( _sizeStorer ) {
            _sizeStorer->onDestroy( this );
            _sizeStorer->store( _uri, _numRecords.load(), _dataSize.load() );
        }
    }

    const char* WiredTigerRecordStore::name() const {
        return kWiredTigerEngineName.c_str();
    }

    long long WiredTigerRecordStore::dataSize( OperationContext *txn ) const {
        return _dataSize.load();
    }

    long long WiredTigerRecordStore::numRecords( OperationContext *txn ) const {
        return _numRecords.load();
    }

    bool WiredTigerRecordStore::isCapped() const {
        return _isCapped;
    }

    int64_t WiredTigerRecordStore::cappedMaxDocs() const {
        invariant(_isCapped);
        return _cappedMaxDocs;
    }

    int64_t WiredTigerRecordStore::cappedMaxSize() const {
        invariant(_isCapped);
        return _cappedMaxSize;
    }

    int64_t WiredTigerRecordStore::storageSize( OperationContext* txn,
                                                BSONObjBuilder* extraInfo,
                                                int infoLevel ) const {
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        StatusWith<int64_t> result = WiredTigerUtil::getStatisticsValueAs<int64_t>(
            session->getSession(),
            "statistics:" + GetURI(), "statistics=(fast)", WT_STAT_DSRC_BLOCK_SIZE);
        uassertStatusOK(result.getStatus());

        int64_t size = result.getValue();

        if ( size == 0 && _isCapped ) {
            // Many things assume an empty capped collection still takes up space.
            return 1;
        }
        return size;
    }

    // Retrieve the value from a positioned cursor.
    RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
        WT_ITEM value;
        int ret = cursor->get_value(cursor.get(), &value);
        invariantWTOK(ret);

        SharedBuffer data = SharedBuffer::allocate(value.size);
        memcpy( data.get(), value.data, value.size );
        return RecordData(data.moveFrom(), value.size);
    }

    RecordData WiredTigerRecordStore::dataFor(OperationContext* txn, const RecordId& loc) const {
        // ownership passes to the shared_array created below
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        massert( 28556,
                 "Didn't find RecordId in WiredTigerRecordStore",
                 ret != WT_NOTFOUND );
        invariantWTOK(ret);
        return _getData(curwrap);
    }

    bool WiredTigerRecordStore::findRecord( OperationContext* txn,
                                            const RecordId& loc, RecordData* out ) const {
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        if ( ret == WT_NOTFOUND )
            return false;
        invariantWTOK(ret);
        *out = _getData(curwrap);
        return true;
    }

    void WiredTigerRecordStore::deleteRecord( OperationContext* txn, const RecordId& loc ) {
        WiredTigerCursor cursor( _uri, _instanceId, txn );
        cursor.assertInActiveTxn();
        WT_CURSOR *c = cursor.get();
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        ret = c->remove(c);
        invariantWTOK(ret);

        _changeNumRecords(txn, false);
        _increaseDataSize(txn, -old_length);
    }

    bool WiredTigerRecordStore::cappedAndNeedDelete() const {
        if (!_isCapped)
            return false;

        if (_dataSize.load() > _cappedMaxSize)
            return true;

        if ((_cappedMaxDocs != -1) && (_numRecords.load() > _cappedMaxDocs))
            return true;

        return false;
    }

    namespace {
        int oplogCounter = 0;
    }

    void WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* txn,
                                                     const RecordId& justInserted ) {

        if ( _isOplog ) {
            if ( oplogCounter++ % 100 > 0 )
                return;
        }

        if (!cappedAndNeedDelete())
            return;

        // ensure only one thread at a time can do deletes, otherwise they'll conflict.
        boost::mutex::scoped_lock lock(_cappedDeleterMutex, boost::try_to_lock);
        if ( !lock )
            return;

        WiredTigerRecoveryUnit* realRecoveryUnit = NULL;
        if ( _isOplog ) {
            // we do this is a sub transaction in case it aborts
            realRecoveryUnit = dynamic_cast<WiredTigerRecoveryUnit*>( txn->releaseRecoveryUnit() );
            invariant( realRecoveryUnit );
            WiredTigerSessionCache* sc = realRecoveryUnit->getSessionCache();
            txn->setRecoveryUnit( new WiredTigerRecoveryUnit( sc ) );
        }

        try {
            WiredTigerCursor curwrap( _uri, _instanceId, txn);
            WT_CURSOR *c = curwrap.get();
            int ret = c->next(c);
            RecordId oldest;
            while ( ret == 0 && cappedAndNeedDelete() ) {
                WriteUnitOfWork wuow( txn );

                invariant(_numRecords.load() > 0);

                uint64_t key;
                ret = c->get_key(c, &key);
                invariantWTOK(ret);
                oldest = _fromKey(key);

                if ( oldest >= justInserted )
                    break;

                if ( _cappedDeleteCallback ) {
                    uassertStatusOK(_cappedDeleteCallback->aboutToDeleteCapped(txn, oldest));
                }

                deleteRecord( txn, oldest );

                ret = c->next(c);

                wuow.commit();
            }

            if (ret != WT_NOTFOUND) invariantWTOK(ret);

        }
        catch ( const WriteConflictException& wce ) {
            if ( _isOplog ) {
                delete txn->releaseRecoveryUnit();
                txn->setRecoveryUnit( realRecoveryUnit );
                log() << "got conflict purging oplog, ignoring";
                return;
            }
            throw;
        }
        catch ( ... ) {
            if ( _isOplog ) {
                delete txn->releaseRecoveryUnit();
                txn->setRecoveryUnit( realRecoveryUnit );
            }
            throw;
        }

        if ( _isOplog ) {
            delete txn->releaseRecoveryUnit();
            txn->setRecoveryUnit( realRecoveryUnit );
        }
    }

    StatusWith<RecordId> WiredTigerRecordStore::extractAndCheckLocForOplog(const char* data,
                                                                          int len) {
        return oploghack::extractKey(data, len);
    }

    StatusWith<RecordId> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                             const char* data,
                                                             int len,
                                                             bool enforceQuota ) {
        if ( _isCapped && len > _cappedMaxSize ) {
            return StatusWith<RecordId>( ErrorCodes::BadValue,
                                       "object to insert exceeds cappedMaxSize" );
        }

        RecordId loc;
        if ( _useOplogHack ) {
            StatusWith<RecordId> status = extractAndCheckLocForOplog(data, len);
            if (!status.isOK())
                return status;
            loc = status.getValue();
            if ( loc > _oplog_highestSeen ) {
                boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
                if ( loc > _oplog_highestSeen ) {
                    _oplog_highestSeen = loc;
                }
            }
        }
        else if ( _isCapped ) {
            boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
            loc = _nextId();
            _addUncommitedDiskLoc_inlock( txn, loc );
        }
        else {
            loc = _nextId();
        }

        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        invariant( c );

        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        int ret = c->insert(c);
        if ( ret ) {
            return StatusWith<RecordId>( wtRCToStatus( ret,
                                                      "WiredTigerRecordStore::insertRecord" ) );
        }

        _changeNumRecords( txn, true );
        _increaseDataSize( txn, len );

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    void WiredTigerRecordStore::dealtWithCappedLoc( const RecordId& loc ) {
        boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
        SortedDiskLocs::iterator it = std::find(_uncommittedDiskLocs.begin(),
                                                _uncommittedDiskLocs.end(),
                                                loc);
        invariant(it != _uncommittedDiskLocs.end());
        _uncommittedDiskLocs.erase(it);
    }

    bool WiredTigerRecordStore::isCappedHidden( const RecordId& loc ) const {
        boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
        if (_uncommittedDiskLocs.empty()) {
            return false;
        }
        return _uncommittedDiskLocs.front() <= loc;
    }

    StatusWith<RecordId> WiredTigerRecordStore::insertRecord( OperationContext* txn,
                                                             const DocWriter* doc,
                                                             bool enforceQuota ) {
        const int len = doc->documentSize();

        boost::shared_array<char> buf( new char[len] );
        doc->writeDocument( buf.get() );

        return insertRecord( txn, buf.get(), len, enforceQuota );
    }

    StatusWith<RecordId> WiredTigerRecordStore::updateRecord( OperationContext* txn,
                                                        const RecordId& loc,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota,
                                                        UpdateMoveNotifier* notifier ) {
        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        invariant( c );
        c->set_key(c, _makeKey(loc));
        int ret = c->search(c);
        invariantWTOK(ret);

        WT_ITEM old_value;
        ret = c->get_value(c, &old_value);
        invariantWTOK(ret);

        int old_length = old_value.size;

        c->set_key(c, _makeKey(loc));
        WiredTigerItem value(data, len);
        c->set_value(c, value.Get());
        ret = c->update(c);
        invariantWTOK(ret);

        _increaseDataSize(txn, len - old_length);

        cappedDeleteAsNeeded(txn, loc);

        return StatusWith<RecordId>( loc );
    }

    Status WiredTigerRecordStore::updateWithDamages( OperationContext* txn,
                                                     const RecordId& loc,
                                                     const RecordData& oldRec,
                                                     const char* damangeSource,
                                                     const mutablebson::DamageVector& damages ) {

        // apply changes to our copy

        std::string data(reinterpret_cast<const char *>(oldRec.data()), oldRec.size());

        char* root = const_cast<char*>( data.c_str() );
        for( size_t i = 0; i < damages.size(); i++ ) {
            mutablebson::DamageEvent event = damages[i];
            const char* sourcePtr = damangeSource + event.sourceOffset;
            char* targetPtr = root + event.targetOffset;
            std::memcpy(targetPtr, sourcePtr, event.size);
        }

        // write back

        WiredTigerItem value(data);

        WiredTigerCursor curwrap( _uri, _instanceId, txn);
        curwrap.assertInActiveTxn();
        WT_CURSOR *c = curwrap.get();
        c->set_key(c, _makeKey(loc));
        c->set_value(c, value.Get());

        int ret = c->update(c);
        invariantWTOK(ret);

        return Status::OK();
    }

    void WiredTigerRecordStore::_oplogSetStartHack( WiredTigerRecoveryUnit* wru ) const {
        boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
        if ( _uncommittedDiskLocs.empty() ) {
            wru->setOplogReadTill( _oplog_highestSeen );
        }
        else {
            wru->setOplogReadTill( _uncommittedDiskLocs.front() );
        }
    }

    RecordIterator* WiredTigerRecordStore::getIterator( OperationContext* txn,
                                                        const RecordId& start,
                                                        const CollectionScanParams::Direction& dir ) const {
        if ( _isOplog && dir == CollectionScanParams::FORWARD ) {
            WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
            if ( !wru->inActiveTxn() || wru->getOplogReadTill().isNull() ) {
                // if we don't have a session, we have no snapshot, so we can update our view
                _oplogSetStartHack( wru );
            }
        }

        return new Iterator(*this, txn, start, dir, false);
    }


    std::vector<RecordIterator*> WiredTigerRecordStore::getManyIterators(
            OperationContext* txn ) const {
        // XXX do we want this to actually return a set of iterators?

        std::vector<RecordIterator*> iterators;
        iterators.push_back( new Iterator(*this, txn, RecordId(),
                                          CollectionScanParams::FORWARD, true) );

        return iterators;
    }

    Status WiredTigerRecordStore::truncate( OperationContext* txn ) {
        // TODO: use a WiredTiger fast truncate
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        while( !iter->isEOF() ) {
            RecordId loc = iter->getNext();
            deleteRecord( txn, loc );
        }

        // WiredTigerRecoveryUnit* ru = _getRecoveryUnit( txn );

        return Status::OK();
    }

    Status WiredTigerRecordStore::compact( OperationContext* txn,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats ) {
        WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(txn)->getSessionCache();
        WiredTigerSession* session = cache->getSession();
        WT_SESSION *s = session->getSession();
        int ret = s->compact(s, GetURI().c_str(), NULL);
        invariantWTOK(ret);
        cache->releaseSession(session);
        return Status::OK();
    }

    Status WiredTigerRecordStore::validate( OperationContext* txn,
                                       bool full, bool scanData,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output ) const {

        long long nrecords = 0;
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn ) );
        results->valid = true;
        while( !iter->isEOF() ) {
            ++nrecords;
            if ( full && scanData ) {
                size_t dataSize;
                RecordId loc = iter->curr();
                RecordData data = dataFor( txn, loc );
                Status status = adaptor->validate( data, &dataSize );
                if ( !status.isOK() ) {
                    results->valid = false;
                    results->errors.push_back( loc.toString() + " is corrupted" );
                }
            }
            iter->getNext();
        }

        output->appendNumber( "nrecords", nrecords );

        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        WT_SESSION* s = session->getSession();
        BSONObjBuilder bob(output->subobjStart(kWiredTigerEngineName));
        Status status = WiredTigerUtil::exportTableToBSON(s, "statistics:" + GetURI(),
                                                          "statistics=(fast)", &bob);
        if (!status.isOK()) {
            bob.append("error", "unable to retrieve statistics");
            bob.append("code", static_cast<int>(status.code()));
            bob.append("reason", status.reason());
        }
        return Status::OK();
    }

    void WiredTigerRecordStore::appendCustomStats( OperationContext* txn,
                                                   BSONObjBuilder* result,
                                                   double scale ) const {
        result->appendBool( "capped", _isCapped );
        if ( _isCapped ) {
            result->appendIntOrLL( "max", _cappedMaxDocs );
            result->appendIntOrLL( "maxSize", _cappedMaxSize );
        }
        WiredTigerSession* session = WiredTigerRecoveryUnit::get(txn)->getSession();
        WT_SESSION* s = session->getSession();
        BSONObjBuilder bob(result->subobjStart(kWiredTigerEngineName));
        Status status = WiredTigerUtil::exportTableToBSON(s, "statistics:" + GetURI(),
                                                          "statistics=(fast)", &bob);
        if (!status.isOK()) {
            bob.append("error", "unable to retrieve statistics");
            bob.append("code", static_cast<int>(status.code()));
            bob.append("reason", status.reason());
        }
    }


    Status WiredTigerRecordStore::touch( OperationContext* txn, BSONObjBuilder* output ) const {
        if (output) {
            output->append("numRanges", 1);
            output->append("millis", 0);
        }
        return Status::OK();
    }

    Status WiredTigerRecordStore::setCustomOption( OperationContext* txn,
                                                   const BSONElement& option,
                                                   BSONObjBuilder* info ) {
        string optionName = option.fieldName();
        if ( !option.isBoolean() ) {
            return Status( ErrorCodes::BadValue, "Invalid Value" );
        }
        // TODO: expose some WiredTiger configurations
        if ( optionName == "usePowerOf2Sizes" ) {
            return Status::OK();
        } else
        if ( optionName.compare( "verify_checksums" ) == 0 ) {
        }
        else
            return Status( ErrorCodes::InvalidOptions, "Invalid Option" );

        return Status::OK();
    }

    Status WiredTigerRecordStore::oplogDiskLocRegister( OperationContext* txn,
                                                        const OpTime& opTime ) {
        StatusWith<RecordId> loc = oploghack::keyForOptime( opTime );
        if ( !loc.isOK() )
            return loc.getStatus();

        boost::mutex::scoped_lock lk( _uncommittedDiskLocsMutex );
        _addUncommitedDiskLoc_inlock( txn, loc.getValue() );
        return Status::OK();
    }

    void WiredTigerRecordStore::_addUncommitedDiskLoc_inlock( OperationContext* txn,
                                                              const RecordId& loc ) {
        // todo: make this a dassert at some point
        invariant( _uncommittedDiskLocs.empty() ||
                   _uncommittedDiskLocs.back() < loc );
        _uncommittedDiskLocs.push_back( loc );
        txn->recoveryUnit()->registerChange( new CappedInsertChange( this, loc ) );
        _oplog_highestSeen = loc;
    }

    RecordId WiredTigerRecordStore::oplogStartHack(OperationContext* txn,
                                                  const RecordId& startingPosition) const {
        if (!_useOplogHack)
            return RecordId().setInvalid();

        {
            WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(txn);
            _oplogSetStartHack( wru );
        }

        WiredTigerCursor cursor(_uri, _instanceId, txn);
        WT_CURSOR* c = cursor.get();

        int cmp;
        c->set_key(c, _makeKey(startingPosition));
        int ret = c->search_near(c, &cmp);
        if (ret == 0 && cmp > 0) ret = c->prev(c); // landed one higher than startingPosition
        if (ret == WT_NOTFOUND) return RecordId(); // nothing <= startingPosition
        invariantWTOK(ret);

        uint64_t key;
        ret = c->get_key(c, &key);
        invariantWTOK(ret);
        return _fromKey(key);
    }

    RecordId WiredTigerRecordStore::_nextId() {
        invariant(!_useOplogHack);
        const uint64_t myId = _nextIdNum.fetchAndAdd(1);
        int a = myId >> 32;
        // This masks the lowest 4 bytes of myId
        int ofs = myId & 0x00000000FFFFFFFF;
        RecordId loc( a, ofs );
        return loc;
    }

    WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit( OperationContext* txn ) {
        return dynamic_cast<WiredTigerRecoveryUnit*>( txn->recoveryUnit() );
    }

    class WiredTigerRecordStore::NumRecordsChange : public RecoveryUnit::Change {
    public:
        NumRecordsChange(WiredTigerRecordStore* rs, bool insert) :_rs(rs), _insert(insert) {}
        virtual void commit() {}
        virtual void rollback() {
            _rs->_numRecords.fetchAndAdd(_insert ? -1 : 1);
        }

    private:
        WiredTigerRecordStore* _rs;
        bool _insert;
    };

    void WiredTigerRecordStore::_changeNumRecords( OperationContext* txn, bool insert ) {
        txn->recoveryUnit()->registerChange(new NumRecordsChange(this, insert));
        if ( _numRecords.fetchAndAdd(insert ? 1 : -1) < 0 ) {
            if ( insert )
                _numRecords.store( 1 );
            else
                _numRecords.store( 0 );
        }
    }

    class WiredTigerRecordStore::DataSizeChange : public RecoveryUnit::Change {
    public:
        DataSizeChange(WiredTigerRecordStore* rs, int amount) :_rs(rs), _amount(amount) {}
        virtual void commit() {}
        virtual void rollback() {
            _rs->_increaseDataSize( NULL, -_amount );
        }

    private:
        WiredTigerRecordStore* _rs;
        bool _amount;
    };

    void WiredTigerRecordStore::_increaseDataSize( OperationContext* txn, int amount ) {
        if ( txn )
            txn->recoveryUnit()->registerChange(new DataSizeChange(this, amount));

        if ( _dataSize.fetchAndAdd(amount) < 0 ) {
            if ( amount > 0 ) {
                _dataSize.store( amount );
            }
            else {
                _dataSize.store( 0 );
            }
        }

        if ( _sizeStorer && _sizeStorerCounter++ % 1000 == 0 ) {
            _sizeStorer->store( _uri, _numRecords.load(), _dataSize.load() );
        }
    }

    uint64_t WiredTigerRecordStore::_makeKey( const RecordId& loc ) {
        return ((uint64_t)loc.a() << 32 | loc.getOfs());
    }
    RecordId WiredTigerRecordStore::_fromKey( uint64_t key ) {
        uint32_t a = key >> 32;
        uint32_t ofs = (uint32_t)key;
        return RecordId(a, ofs);
    }

    // --------

    WiredTigerRecordStore::Iterator::Iterator(
            const WiredTigerRecordStore& rs,
            OperationContext *txn,
            const RecordId& start,
            const CollectionScanParams::Direction& dir,
            bool forParallelCollectionScan)
        : _rs( rs ),
          _txn( txn ),
          _forward( dir == CollectionScanParams::FORWARD ),
          _forParallelCollectionScan( forParallelCollectionScan ),
          _cursor( new WiredTigerCursor( rs.GetURI(), rs.instanceId(), txn ) ),
          _eof(false),
          _readUntilForOplog(WiredTigerRecoveryUnit::get(txn)->getOplogReadTill()) {
        RS_ITERATOR_TRACE("start");
        _locate(start, true);
    }

    WiredTigerRecordStore::Iterator::~Iterator() {
    }

    void WiredTigerRecordStore::Iterator::_locate(const RecordId &loc, bool exact) {
        RS_ITERATOR_TRACE("_locate " << loc);
        WT_CURSOR *c = _cursor->get();
        invariant( c );
        int ret;
        if (loc.isNull()) {
            ret = _forward ? c->next(c) : c->prev(c);
            _eof = (ret == WT_NOTFOUND);
            if (!_eof) invariantWTOK(ret);
            _loc = _curr();

            RS_ITERATOR_TRACE("_locate   null loc eof: " << _eof);
            return;
        }

        c->set_key(c, _makeKey(loc));
        if (exact) {
            ret = c->search(c);
        }
        else {
            // If loc doesn't exist, inexact matches should find the first existing record before
            // it, in the direction of the scan. Note that inexact callers will call _getNext()
            // after locate so they actually return the record *after* the one we seek to.
            int cmp;
            ret = c->search_near(c, &cmp);
            if ( ret == WT_NOTFOUND ) {
                _eof = true;
                _loc = RecordId();
                return;
            }
            invariantWTOK(ret);
            if (_forward) {
                // return >= loc
                if (cmp < 0)
                    ret = c->next(c);
            }
            else {
                // return <= loc
                if (cmp > 0)
                    ret = c->prev(c);
            }
        }
        if (ret != WT_NOTFOUND) invariantWTOK(ret);
        _eof = (ret == WT_NOTFOUND);
        _loc = _curr();
        RS_ITERATOR_TRACE("_locate   not null loc eof: " << _eof);
    }

    bool WiredTigerRecordStore::Iterator::isEOF() {
        RS_ITERATOR_TRACE( "isEOF " << _eof << " " << _lastLoc );
        return _eof;
    }

    // Allow const functions to use curr to find current location.
    RecordId WiredTigerRecordStore::Iterator::_curr() const {
        RS_ITERATOR_TRACE( "_curr" );
        if (_eof)
            return RecordId();

        WT_CURSOR *c = _cursor->get();
        dassert( c );
        uint64_t key;
        int ret = c->get_key(c, &key);
        invariantWTOK(ret);
        return _fromKey(key);
    }

    RecordId WiredTigerRecordStore::Iterator::curr() {
        return _loc;
    }

    void WiredTigerRecordStore::Iterator::_getNext() {
        // Once you go EOF you never go back.
        if (_eof) return;

        RS_ITERATOR_TRACE("_getNext");
        WT_CURSOR *c = _cursor->get();
        int ret = _forward ? c->next(c) : c->prev(c);
        _eof = (ret == WT_NOTFOUND);
        RS_ITERATOR_TRACE("_getNext " << ret << " " << _eof );
        if ( !_eof ) {
            RS_ITERATOR_TRACE("_getNext " << ret << " " << _eof << " " << _curr() );
            invariantWTOK(ret);
            _loc = _curr();
            RS_ITERATOR_TRACE("_getNext " << ret << " " << _eof << " " << _loc );
            if ( _rs._isCapped ) {
                RecordId loc = _curr();
                if ( _readUntilForOplog.isNull() ) {
                    // this is the normal capped case
                    if ( _rs.isCappedHidden( loc ) ) {
                        _eof = true;
                    }
                }
                else {
                    // this is for oplogs
                    if ( loc > _readUntilForOplog ) {
                        _eof = true;
                    }
                    else if ( loc == _readUntilForOplog && _rs.isCappedHidden( loc ) ) {
                        // we allow if its been commited already
                        _eof = true;
                    }
                }
            }
        }

        if (_eof) {
            _loc = RecordId();
        }
    }

    RecordId WiredTigerRecordStore::Iterator::getNext() {
        RS_ITERATOR_TRACE( "getNext" );
        const RecordId toReturn = _loc;
        RS_ITERATOR_TRACE( "getNext toReturn: " << toReturn );
        _getNext();
        RS_ITERATOR_TRACE( " ----" );
        _lastLoc = toReturn;
        return toReturn;
    }

    void WiredTigerRecordStore::Iterator::invalidate( const RecordId& dl ) {
        // this should never be called
    }

    void WiredTigerRecordStore::Iterator::saveState() {
        RS_ITERATOR_TRACE("saveState");

        // the cursor and recoveryUnit are valid on restore
        // so we just record the recoveryUnit to make sure
        _savedRecoveryUnit = _txn->recoveryUnit();
        if ( !wt_keeptxnopen() ) {
            _cursor->reset();
        }

        if ( _forParallelCollectionScan ) {
            _cursor.reset( NULL );
        }
        _txn = NULL;
    }

    bool WiredTigerRecordStore::Iterator::restoreState( OperationContext *txn ) {

        // This is normally already the case, but sometimes we are given a new
        // OperationContext on restore - update the iterators context in that
        // case
        _txn = txn;

        // If we've hit EOF, then this iterator is done and need not be restored.
        if ( _eof ) {
            return true;
        }

        bool needRestore = false;

        if ( _forParallelCollectionScan ) {
            // parallel collection scan or something
            needRestore = true;
            _savedRecoveryUnit = txn->recoveryUnit();
            _cursor.reset( new WiredTigerCursor( _rs.GetURI(), _rs.instanceId(), txn ) );
            _forParallelCollectionScan = false; // we only do this the first time
        }

        invariant( _savedRecoveryUnit == txn->recoveryUnit() );
        if ( needRestore || !wt_keeptxnopen() ) {
            RecordId saved = _lastLoc;
            _locate(_lastLoc, false);
            RS_ITERATOR_TRACE( "isEOF check " << _eof );
            if ( _eof ) {
                _lastLoc = RecordId();
            }
            else if ( _loc != saved ) {
                // old doc deleted, we're ok
            }
            else {
                // we found where we left off!
                // now we advance to the next one
                RS_ITERATOR_TRACE( "isEOF found " << _curr() );
                _getNext();
            }
        }

        return true;
    }

    RecordData WiredTigerRecordStore::Iterator::dataFor( const RecordId& loc ) const {
        // Retrieve the data if the iterator is already positioned at loc, otherwise
        // open a new cursor and find the data to avoid upsetting the iterators
        // cursor position.
        if (loc == _loc) {
            dassert(loc == _curr());
            return _rs._getData(*_cursor);
        }
        else {
            return _rs.dataFor( _txn, loc );
        }
    }

    void WiredTigerRecordStore::temp_cappedTruncateAfter( OperationContext* txn,
                                                     RecordId end,
                                                     bool inclusive ) {
        WriteUnitOfWork wuow(txn);
        boost::scoped_ptr<RecordIterator> iter( getIterator( txn, end ) );
        while( !iter->isEOF() ) {
            RecordId loc = iter->getNext();
            if ( end < loc || ( inclusive && end == loc ) ) {
                deleteRecord( txn, loc );
            }
        }
        wuow.commit();
    }
}
