//===- PersistentPointsToCache.h -- Persistent points-to sets ----------------//

/*
 * PersistentPointsToCache.h
 *
 * The implementation is based on
 * Mohamad Barbar and Yulei Sui. Hash Consed Points-To Sets.
 * 28th Static Analysis Symposium (SAS'21)
 *
 *  Created on: Sep 28, 2020
 *      Author: Mohamad Barbar
 */

#ifndef MDE_PERSISTENT_POINTS_TO_CACHE_H_
#define MDE_PERSISTENT_POINTS_TO_CACHE_H_

#include <iostream>
#include <lhf/lhf_common.hpp>
#include "MemoryModel/MDEPointee.h"
#include "Util/SVFUtil.h"

namespace SVF
{

/// Persistent points-to set store. Can be used as a backing for points-to data structures like
/// PointsToDS and PointsToDFDS. Hides points-to sets and union operations from users and hands
/// out PointsToIDs.
/// Points-to sets are interned, and union operations are lazy and hash-consed.
template <typename Data>
class PersistentPointsToCache
{
protected:
    lhf::PointeeMDE pointeeMDE;

public:

    static PointsToID emptyPointsToId(void)
    {
        return lhf::EMPTY_SET_VALUE;
    };

public:
    PersistentPointsToCache(void)
    {
    }

    /// Clear the cache.
    void clear()
    {
        pointeeMDE.clear();
    }

    /// Resets the cache removing everything except the emptyData it was initialised with.
    void reset(void)
    {
        pointeeMDE.clear_and_initialize();
    }

    /// Remaps all points-to sets stored in the cache to the current mapping.
    void remapAllPts(void)
    {
        pointeeMDE.PointsTo_checkAndRemapAll();
    }

    /// If pts is not in the PersistentPointsToCache, inserts it, assigns an ID, and returns
    /// that ID. If it is, then the ID is returned.
    PointsToID emplacePts(const Data &pts)
    {
        return pointeeMDE.register_set(pts);
    }

    /// Returns the points-to set which id represents. id must be stored in the cache.
    const Data &getActualPts(PointsToID id) const
    {
        // Check if the points-to set for ID has already been stored.
        // assert(idToPts.size() > id && "PPTC::getActualPts: points-to set not stored!");
        return pointeeMDE.get_value(id);
    }

    /// Unions lhs and rhs and returns their union's ID.
    PointsToID unionPts(PointsToID lhs, PointsToID rhs)
    {
        return pointeeMDE.set_union(lhs, rhs).value;
    }

    /// Relatively complements lhs and rhs (lhs \ rhs) and returns it's ID.
    PointsToID complementPts(PointsToID lhs, PointsToID rhs)
    {
        return pointeeMDE.set_difference(lhs, rhs).value;
    }

    /// Intersects lhs and rhs (lhs AND rhs) and returns the intersection's ID.
    PointsToID intersectPts(PointsToID lhs, PointsToID rhs)
    {
        return pointeeMDE.set_intersection(lhs, rhs).value;
    }

    /// Print statistics on operations and points-to set numbers.
    void printStats(const std::string subtitle) const
    {
        SVFUtil::outs() << pointeeMDE.dump_perf();
        SVFUtil::outs().flush();
    }

    /// Returns all points-to sets stored by this cache as keys to a map.
    /// Values are all 1. We use the map to be more compatible with getAllPts
    /// in the various PTDatas. Performance is a non-issue (for now) since this
    /// is just used for evaluation's sake.
    Map<Data, unsigned> getAllPts(void)
    {
        return pointeeMDE.PointsTo_getAllPts();
    }

    // TODO: ref count API for garbage collection.
};

} // End namespace SVF

#endif /* PERSISTENT_POINTS_TO_H_ */
