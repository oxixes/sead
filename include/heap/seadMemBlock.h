#pragma once

#include "basis/seadTypes.h"
#include "container/seadListImpl.h"
#include "container/seadOffsetList.h"
#include "prim/seadPtrUtil.h"

namespace sead
{
class MemBlock
{
public:
    MemBlock() : mHeapCheckTag(0), mOffset(0), mSize(0) {}

    static MemBlock* FindManageArea(void* ptr);

    static u32 getOffset() { return offsetof(MemBlock, mListNode); }

    /// Start of the block's data, after the header and any alignment padding.
    u8* memory() const
    {
        return static_cast<u8*>(PtrUtil::addOffset(this, sizeof(MemBlock) + mOffset));
    }

protected:
    ListNode mListNode;
    u16 mHeapCheckTag;
    u16 mOffset;
    size_t mSize;

private:
    friend class ExpHeap;
};

// The word just before the data is either the block's mSize, or a pointer to the block if there is
// alignment padding between the block and the data. Sizes and blocks are always aligned to
// sizeof(void*), so the lowest bit of a size is always 0; it is set to 1 to mark a pointer.
inline MemBlock* MemBlock::FindManageArea(void* ptr)
{
    const uintptr_t tag =
        *static_cast<uintptr_t*>(PtrUtil::addOffset(ptr, -static_cast<intptr_t>(sizeof(void*))));
    const bool is_ptr = tag & 1;

    return is_ptr ? reinterpret_cast<MemBlock*>(tag - 1) :
                    static_cast<MemBlock*>(
                        PtrUtil::addOffset(ptr, -static_cast<intptr_t>(sizeof(MemBlock))));
}

using MemBlockList = OffsetList<MemBlock>;
}  // namespace sead
