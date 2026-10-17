#include <heap/seadExpHeap.h>
#include <heap/seadHeapMgr.h>
#include <heap/seadMemBlock.h>
#include <math/seadMathCalcCommon.hpp>

namespace sead
{
ExpHeap::ExpHeap(const SafeString& name, Heap* parent, void* address, size_t size,
                 HeapDirection direction, bool enable_lock)
    : Heap(name, parent, address, size, direction, enable_lock), mAllocMode(AllocMode::FirstFit),
      mFindFreeBlockMode(FindFreeBlockMode::Auto)
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    mFreeList.initOffset(static_cast<s32>(MemBlock::getOffset()));
    mUseList.initOffset(static_cast<s32>(MemBlock::getOffset()));
}

ExpHeap::~ExpHeap()
{
    Heap::destruct_();
}

ExpHeap* ExpHeap::create(size_t size, const SafeString& name, Heap* parent, s32 alignment,
                         HeapDirection direction, bool enable_lock)
{
    auto* heap = tryCreate(size, name, parent, alignment, direction, enable_lock);
    SEAD_ASSERT_MSG(heap, "ExpHeap::create failed. size: %zu, alignment: %d, name: %s", size,
                    alignment, name.cstr());
    return heap;
}

/* ExpHeap* ExpHeap::tryCreate(size_t size, const SafeString& name, Heap* parent, s32 alignment,
                            HeapDirection direction, bool enable_lock)
{
    if (parent == nullptr)
    {
        parent = HeapMgr::instance()->getCurrentHeap();
        if (parent == nullptr)
            return nullptr;
    }

    if (size == 0)
        size = parent->getMaxAllocatableSize(alignment) / sizeof(void*) * sizeof(void*);
    else
        size = MathSizeT::roundUp(size, sizeof(void*));

    if (size < sizeof(ExpHeap))
        return nullptr;
    if ((Mathi::abs(alignment) - 1) & Mathi::abs(alignment))
        return nullptr;

    void* mem = parent->tryAlloc(size, direction * alignment);
    if (mem == nullptr)
        return nullptr;

    FrameHeap* heap;

    auto heap_direction = static_cast<HeapDirection>(
        parent->mDirection == cHeapDirection_Reverse ? -direction : direction);
    if (heap_direction == cHeapDirection_Forward)
        heap = new (mem) FrameHeap(name, parent, mem, size, cHeapDirection_Forward, enable_lock);
    else
        heap = new (PtrUtil::addOffset(mem, static_cast<intptr_t>(size - sizeof(FrameHeap))))
            FrameHeap(name, parent, mem, size, heap_direction, enable_lock);
} */

ExpHeap* ExpHeap::create(void* address, size_t size, const SafeString& name, Heap* parent,
                         bool enable_lock)
{
    auto* heap = tryCreate(address, size, name, parent, enable_lock);
    SEAD_ASSERT_MSG(heap, "ExpHeap::create failed. size: %zu, name: %s", size, name.cstr());
    return heap;
}

size_t ExpHeap::getManagementAreaSize(s32 alignment)
{
    return alignment + sizeof(ExpHeap) + sizeof(MemBlock);
}

void ExpHeap::destroy()
{
    destroyAndGetAllocatableSize(sizeof(void*));
}

bool ExpHeap::isInclude(const void* p_void) const
{
    const void* start;
    const void* end;
    if (mDirection == cHeapDirection_Forward)
    {
        start = PtrUtil::addOffset(mStart, sizeof(ExpHeap));
        end = PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize));
    }
    else
    {
        start = mStart;
        end = PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize - sizeof(ExpHeap)));
    }

    return start <= p_void && p_void < end;
}

void ExpHeap::free(void* ptr)
{
    freeAndGetAllocatableSize(ptr, sizeof(void*));
}

uintptr_t ExpHeap::getStartAddress() const
{
    return reinterpret_cast<uintptr_t>(mStart);
}

uintptr_t ExpHeap::getEndAddress() const
{
    return reinterpret_cast<uintptr_t>(mStart) + mSize;
}

size_t ExpHeap::getSize() const
{
    return mSize;
}

size_t ExpHeap::getFreeSize() const
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    size_t free_size = 0;
    for (auto mem_block : mFreeList)
        free_size += mem_block.mSize;
    return free_size;
}

s32 ExpHeap::compareMemBlockAddr_(const MemBlock* a, const MemBlock* b)
{
    s32 diff = static_cast<s32>(PtrUtil::diff(a, b) & 0xFFFFFFFF);
    return diff < 0 ? -1 : 1;
}

size_t ExpHeap::getAllocatedSize(void* object)
{
    if (!isInclude(object))
        return 0;

    // The previous word is the size of the block (always aligned to sizeof(void*)) or a pointer to the block.
    // To indicate which one it is, the least significant bit is used as a flag. If the size is odd, it means the value is not the actual size of the block, but rather a pointer to the block.
    // This is because, in all cases, the lowest bit of the size is always 0, since the size and blocks are always aligned to sizeof(void*).
    uintptr_t tag = *reinterpret_cast<uintptr_t*>(PtrUtil::addOffset(object, -static_cast<intptr_t>(sizeof(void*))));
    bool is_ptr = tag & 1;

    MemBlock* memBlock = (is_ptr) ?
        reinterpret_cast<MemBlock*>(tag - 1) /* Remove the flag bit */ :
        reinterpret_cast<MemBlock*>(PtrUtil::addOffset(object, -static_cast<intptr_t>(sizeof(MemBlock))));
    return memBlock->mSize;
}

void ExpHeap::setFindFreeBlockMode(FindFreeBlockMode mode)
{
    mFindFreeBlockMode = mode;
}

void ExpHeap::genInformation_(hostio::Context* context)
{
    Heap::genInformation_(context);
}

bool ExpHeap::isEmpty() const
{
    return this->mUseList.size() == 0;
}

bool ExpHeap::isFreeable() const
{
    return true;
}

bool ExpHeap::isResizable() const
{
    return true;
}

bool ExpHeap::isAdjustable() const
{
    return true;
}
}  // namespace sead