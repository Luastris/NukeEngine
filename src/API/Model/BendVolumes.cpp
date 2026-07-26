#include "API/Model/BendVolumes.h"
#include <mutex>

namespace nuke {

static std::mutex gBendLock;
static std::vector<BendVolume> gBendPending;

void BendVolumes::Submit(const BendVolume& v)
{
	std::lock_guard<std::mutex> l(gBendLock);
	if (gBendPending.size() < 64) gBendPending.push_back(v);
}

std::vector<BendVolume> BendVolumes::Consume()
{
	std::lock_guard<std::mutex> l(gBendLock);
	std::vector<BendVolume> out;
	out.swap(gBendPending);
	return out;
}

}  // namespace nuke
