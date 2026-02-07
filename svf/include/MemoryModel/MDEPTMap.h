#ifndef MDE_PTMAP_H_
#define MDE_PTMAP_H_
#include "MemoryModel/MDEPointee.h"
#include "PointsTo.h"
#include "Util/GeneralType.h"
#define LHF_ENABLE_PERFORMANCE_METRICS
#include <lhf/lhf_common.hpp>
#include <lhf/lhf.hpp>

namespace lhf {

class PTMapMDE :
	public LatticeHashForest<
		LHFConfig<SVF::NodeID>,
		NestingBase<SVF::NodeID, PointeeMDE>> {

};

}; // END NAMESPACE

#endif