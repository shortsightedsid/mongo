/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/write_concern.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

//#define REPLDEBUG(x) log() << "replBlock: "  << x << endl;
#define REPLDEBUG(x)

namespace mongo {
namespace repl {

    using namespace mongoutils;

    class SlaveTracking { // SERVER-4328 todo review
    public:

        struct Ident {

            Ident(const BSONObj& r, const BSONObj& config) {
                BSONObjBuilder b;
                b.appendElements( r );
                b.append( "config" , config );
                obj = b.obj();
            }

            bool operator<( const Ident& other ) const {
                return obj["_id"].OID() < other.obj["_id"].OID();
            }

            BSONObj obj;
        };

        SlaveTracking() : _mutex("SlaveTracking") {
        }

        void reset() {
            scoped_lock mylk(_mutex);
            _slaves.clear();
        }

        bool update(const BSONObj& rid, const BSONObj config, OpTime last) {
            REPLDEBUG( config << " " << rid << " " << ns << " " << last );

            Ident ident(rid, config);

            scoped_lock mylk(_mutex);

            if (last > _slaves[ident]) {
                _slaves[ident] = last;

                // update write concern tags if this node is primary
                if (theReplSet && theReplSet->isPrimary()) {
                    const Member* mem = theReplSet->findById(ident.obj["config"]["_id"].Int());
                    if (!mem) {
                        return false;
                    }
                    ReplSetConfig::MemberCfg cfg = mem->config();
                    cfg.updateGroups(last);
                }

                _threadsWaitingForReplication.notify_all();
            }
            return true;
        }

        bool opReplicatedEnough( OpTime op , BSONElement w ) {
            RARELY {
                REPLDEBUG( "looking for : " << op << " w=" << w );
            }

            if (w.isNumber()) {
                return replicatedToNum(op, w.numberInt());
            }

            uassert( 16250 , "w has to be a string or a number" , w.type() == String );

            if (!theReplSet) {
                return false;
            }

            return opReplicatedEnough( op, w.String() );
        }

        bool opReplicatedEnough( OpTime op , const string& wStr ) {
            if (wStr == "majority") {
                // use the entire set, including arbiters, to prevent writing
                // to a majority of the set but not a majority of voters
                return replicatedToNum(op, theReplSet->config().getMajority());
            }

            map<string,ReplSetConfig::TagRule*>::const_iterator it = theReplSet->config().rules.find(wStr);
            uassert(ErrorCodes::UnknownReplWriteConcern,
                    str::stream() << "unrecognized getLastError mode: " << wStr,
                    it != theReplSet->config().rules.end());

            return op <= (*it).second->last;
        }

        bool replicatedToNum(OpTime& op, int w) {
            massert(ErrorCodes::NotMaster,
                    "replicatedToNum called but not master anymore",
                    getGlobalReplicationCoordinator()->getReplicationMode() !=
                            ReplicationCoordinator::modeReplSet ||
                            getGlobalReplicationCoordinator()->getCurrentMemberState().primary());

            if ( w <= 1 )
                return true;

            w--; // now this is the # of slaves i need
            scoped_lock mylk(_mutex);
            return _replicatedToNum_slaves_locked( op, w );
        }

        bool waitForReplication(OpTime& op, int w, int maxSecondsToWait) {
            static const int noLongerMasterAssertCode = ErrorCodes::NotMaster;
            massert(noLongerMasterAssertCode, 
                    "waitForReplication called but not master anymore",
                    getGlobalReplicationCoordinator()->getReplicationMode() != 
                            ReplicationCoordinator::modeReplSet ||
                            getGlobalReplicationCoordinator()->getCurrentMemberState().primary());

            if ( w <= 1 )
                return true;

            w--; // now this is the # of slaves i need

            boost::xtime xt;
            boost::xtime_get(&xt, MONGO_BOOST_TIME_UTC);
            xt.sec += maxSecondsToWait;
            
            ReplicationCoordinator* replCoord = getGlobalReplicationCoordinator();
            scoped_lock mylk(_mutex);
            while ( ! _replicatedToNum_slaves_locked( op, w ) ) {
                if ( ! _threadsWaitingForReplication.timed_wait( mylk.boost() , xt ) ) {
                    massert(noLongerMasterAssertCode,
                            "waitForReplication called but not master anymore",
                            replCoord->getReplicationMode() != ReplicationCoordinator::modeReplSet
                                    || replCoord->getCurrentMemberState().primary());
                    return false;
                }
                massert(noLongerMasterAssertCode, 
                        "waitForReplication called but not master anymore",
                        replCoord->getReplicationMode() != ReplicationCoordinator::modeReplSet
                                || replCoord->getCurrentMemberState().primary());
            }
            return true;
        }

        bool _replicatedToNum_slaves_locked(OpTime& op, int numSlaves ) {
            for ( map<Ident,OpTime>::iterator i=_slaves.begin(); i!=_slaves.end(); i++) {
                OpTime s = i->second;
                if ( s < op ) {
                    continue;
                }
                if ( --numSlaves == 0 )
                    return true;
            }
            return numSlaves <= 0;
        }

        std::vector<BSONObj> getHostsAtOp(const OpTime& op) {
            std::vector<BSONObj> result;
            if (theReplSet) {
                result.push_back(theReplSet->myConfig().asBson());
            }

            scoped_lock mylk(_mutex);
            for (map<Ident,OpTime>::iterator i = _slaves.begin(); i != _slaves.end(); i++) {
                OpTime replicatedTo = i->second;
                if (replicatedTo >= op) {
                    result.push_back(i->first.obj["config"].Obj());
                }
            }

            return result;
        }

        unsigned getSlaveCount() const {
            scoped_lock mylk(_mutex);

            return _slaves.size();
        }

        // need to be careful not to deadlock with this
        mutable mongo::mutex _mutex;
        boost::condition _threadsWaitingForReplication;

        map<Ident,OpTime> _slaves;

    } slaveTracking;

    bool updateSlaveTracking(const BSONObj& rid,
                             const BSONObj config,
                             OpTime last) {
        return slaveTracking.update(rid, config, last);
    }

    bool opReplicatedEnough( OpTime op , BSONElement w ) {
        return slaveTracking.opReplicatedEnough( op , w );
    }

    bool opReplicatedEnough( OpTime op , int w ) {
        return slaveTracking.replicatedToNum( op , w );
    }

    bool opReplicatedEnough( OpTime op , const string& w ) {
        return slaveTracking.opReplicatedEnough( op , w );
    }

    bool waitForReplication( OpTime op , int w , int maxSecondsToWait ) {
        return slaveTracking.waitForReplication( op, w, maxSecondsToWait );
    }

    vector<BSONObj> getHostsWrittenTo( const OpTime& op ) {
        return slaveTracking.getHostsAtOp(op);
    }

    void resetSlaveCache() {
        slaveTracking.reset();
    }

    unsigned getSlaveCount() {
        return slaveTracking.getSlaveCount();
    }
} // namespace repl
} // namespace mongo
