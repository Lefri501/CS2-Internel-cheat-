#include "utlhash.h"

int CUtlMemoryPool::Count() const {
	return nBlocksAllocated;
}

int CUtlMemoryPool::PeakCount() const {
	return nPeakAlloc;
}

int CUtlMemoryPool::BlockSize() const {
	return nBlockSize;
}