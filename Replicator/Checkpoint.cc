//
// Checkpoint.cc
//
// Copyright 2019-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Checkpoint.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include "fleece/Expert.hh"
#include <limits>
#include <sstream>

#define SPARSE_CHECKPOINTS  // If defined, save entire sparse set to JSON

namespace litecore::repl {
    using namespace std;
    using namespace fleece;


    bool Checkpoint::gWriteTimestamps = true;

    void Checkpoint::resetLocal() {
        _completed.clear();
        _completed.add(0_seq, 1_seq);
        _lastChecked = 0_seq;
    }

    alloc_slice Checkpoint::toJSON() const {
        JSONEncoder enc;
        enc.beginDict();
        if ( gWriteTimestamps ) {
            enc.writeKey("time"_sl);
            enc.writeInt(int64_t(c4_now()) / 1000);
        }

        auto minSeq = localMinSequence();
        if ( minSeq > 0_seq ) {
            enc.writeKey("local"_sl);
            enc.writeUInt(uint64_t(minSeq));
        }

#ifdef SPARSE_CHECKPOINTS
        if ( _completed.rangesCount() > 1 ) {
            // New property for sparse checkpoint. Write the pending sequence ranges as
            // (sequence, length) pairs in an array, omitting the 'infinity' at the end of the last.
            enc.writeKey("localCompleted"_sl);
            enc.beginArray();
            for ( auto& range : _completed ) {
                enc.writeUInt(uint64_t(range.first));
                enc.writeUInt(uint64_t(range.second - range.first));
            };
            enc.endArray();
        }
#endif

        if ( _remote ) {
            enc.writeKey("remote"_sl);
            expert(enc).writeRaw(_remote.toJSON());
        }

        enc.endDict();
        return enc.finish();
    }

    void Checkpoint::readJSON(slice json) {
        Doc root;
        if ( json ) {
            root = Doc::fromJSON(json, nullptr);
            if ( !root ) LogError(SyncLog, "Unparseable checkpoint: %.*s", SPLAT(json));
        }
        readDict(root);
    }

    void Checkpoint::readDict(Dict root) {
        resetLocal();
        _remote = {};

        if ( !root ) return;

        _remote = RemoteSequence(root["remote"_sl]);

#ifdef SPARSE_CHECKPOINTS
        // New properties for sparse checkpoint:
        Array pending = root["localCompleted"].asArray();
        if ( pending ) {
            for ( Array::iterator i(pending); i; ++i ) {
                auto first = C4SequenceNumber(i->asUnsigned());
                auto last  = C4SequenceNumber((++i)->asUnsigned());
                _completed.add(first, first + (uint64_t)last);
            }
        } else
#endif
        {
            auto minSequence = (C4SequenceNumber)root["local"_sl].asInt();
            _completed.add(0_seq, minSequence + 1);
        }
    }

    bool Checkpoint::validateWith(const Checkpoint& remoteSequences) {
        // If _completed or _remote changes in any way because of this method, this method must
        // return false.  Currently the only way this remains true is if neither of the below if
        // blocks are entered, or if we ignore the fact that the only difference in the local
        // and remote checkpoint documents are that the INTEGRAL (i.e. NOT a backfill checkpoint)
        // remote sequence on the local side is older than on the remote side and all we need
        // to do is ignore the remote checkpoint and use the local checkpoint as-is.
        bool match = true;

        if ( _completed != remoteSequences._completed ) {
            LogTo(SyncLog, "Local sequence mismatch: I had completed: %s, remote had %s.",
                  _completed.to_string().c_str(), remoteSequences._completed.to_string().c_str());
            LogTo(SyncLog, "Rolling back to a failsafe, some redundant changes may be proposed...");
            _completed = SequenceSet::intersection(_completed, remoteSequences._completed);
            match      = false;
        }
        if ( _remote && _remote != remoteSequences._remote ) {
            LogTo(SyncLog, "Remote sequence mismatch: I had '%s', remote had '%s'", _remote.toJSONString().c_str(),
                  remoteSequences._remote.toJSONString().c_str());
            if ( _remote.isInt() && remoteSequences._remote.isInt() ) {
                if ( _remote.intValue() > remoteSequences._remote.intValue() ) {
                    LogTo(SyncLog, "Rolling back to earlier remote sequence from server, some redundant changes may be "
                                   "proposed...");
                    _remote = remoteSequences._remote;
                    match   = false;
                } else {
                    LogTo(SyncLog, "Ignoring remote sequence on server since client side is older, some redundant "
                                   "changes may be proposed...");
                }
            } else {
                Warn("Non-numeric remote sequence detected, resetting replication back to start.  Redundant changes "
                     "will be proposed...");
                _remote = {};
                match   = false;
            }
        }

        return match;
    }

    C4SequenceNumber Checkpoint::localMinSequence() const {
        assert(!_completed.empty());
        return _completed.begin()->second - 1;
    }

    void Checkpoint::addPendingSequence(C4SequenceNumber s) {
        _lastChecked = max(_lastChecked, s);
        _completed.remove(s);
    }

    size_t Checkpoint::pendingSequenceCount() const {
        // Count the gaps between the ranges:
        size_t           count = 0;
        C4SequenceNumber end   = 0_seq;
        for ( auto& range : _completed ) {
            count += range.first - end;
            end = range.second;
        }
        if ( _lastChecked > end - 1 ) count += _lastChecked - (end - 1);
        return count;
    }

    bool Checkpoint::setRemoteMinSequence(const RemoteSequence& s) {
        if ( s == _remote ) return false;
        _remote = s;
        return true;
    }

}  // namespace litecore::repl

namespace litecore {

    // The one SequenceSet method I didn't want in the header (because it drags in <stringstream>)

    std::string SequenceSet::to_string() const {
        std::stringstream str;
        str << "[";
        int n = 0;
        for ( auto& range : _sequences ) {
            if ( n++ > 0 ) str << ", ";
            str << uint64_t(range.first);
            if ( range.second != range.first + 1 ) str << "-" << uint64_t((range.second - 1));
        }
        str << "]";
        return str.str();
    }

}  // namespace litecore
