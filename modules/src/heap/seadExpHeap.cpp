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

ExpHeap* ExpHeap::tryCreate(size_t size, const SafeString& name, Heap* parent, s32 alignment,
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

    if (size <= sizeof(ExpHeap) + sizeof(MemBlock))
        return nullptr;
    if ((Mathi::abs(alignment) - 1) & Mathi::abs(alignment))
        return nullptr;

    void* mem = parent->tryAlloc(size, direction * alignment);
    if (mem == nullptr)
        return nullptr;

    ExpHeap* heap;

    auto heap_direction = static_cast<HeapDirection>(
        parent->mDirection == cHeapDirection_Reverse ? -direction : direction);
    if (heap_direction == cHeapDirection_Forward)
        heap = new (mem) ExpHeap(name, parent, mem, size, cHeapDirection_Forward, enable_lock);
    else
        heap = new (PtrUtil::addOffset(mem, static_cast<intptr_t>(size - sizeof(ExpHeap))))
            ExpHeap(name, parent, mem, size, heap_direction, enable_lock);

    doCreate(heap, parent);
    return heap;
}

void ExpHeap::doCreate(ExpHeap* heap, Heap* parent)
{
    createMaxSizeFreeMemBlock_(heap);
    if (parent)
        parent->pushBackChild_(heap);
}

ExpHeap* ExpHeap::tryCreate(void* address, size_t size, const SafeString& name, bool enable_lock)
{
    size_t rounded_size = size / sizeof(void*) * sizeof(void*);
    if (rounded_size <= sizeof(ExpHeap) + sizeof(MemBlock))
        return nullptr;

    auto* heap = new (address)
        ExpHeap(name, nullptr, address, rounded_size, cHeapDirection_Forward, enable_lock);

    doCreate(heap, nullptr);
    return heap;
}

ExpHeap* ExpHeap::create(void* address, size_t size, const SafeString& name, Heap* parent,
                         bool enable_lock)
{
    auto* heap = tryCreate(address, size, name, parent, enable_lock);
    SEAD_ASSERT_MSG(heap, "ExpHeap::create failed. size: %zu, name: %s", size, name.cstr());
    return heap;
}

ExpHeap* ExpHeap::tryCreate(void* address, size_t size, const SafeString& name, Heap* parent,
                            bool enable_lock)
{
    if (parent == nullptr || !parent->isInclude(address) ||
        !parent->isInclude(PtrUtil::addOffset(address, static_cast<intptr_t>(size) - 1)))
        return nullptr;

    if (!PtrUtil::isAligned(address, sizeof(void*)))
        return nullptr;

    if (size <= sizeof(ExpHeap) + sizeof(MemBlock) || size % sizeof(void*) != 0)
        return nullptr;

    auto* heap =
        new (address) ExpHeap(name, parent, address, size, cHeapDirection_Forward, enable_lock);
    doCreate(heap, parent);

    // The memory belongs to the caller, so destroying the heap must not free it to the parent
    heap->mFlag.setBit(Heap::Flag::cExternalMemory);

    return heap;
}

size_t ExpHeap::getManagementAreaSize(s32 alignment)
{
    return alignment + sizeof(ExpHeap) + sizeof(MemBlock);
}

void ExpHeap::createMaxSizeFreeMemBlock_(ExpHeap* heap)
{
    ConditionalScopedLock<CriticalSection> lock(&heap->mCS, heap->isLockEnabled());

    MemBlock* mem_block;
    if (heap->mDirection == cHeapDirection_Forward)
        mem_block = new (PtrUtil::addOffset(heap, sizeof(ExpHeap))) MemBlock();
    else
        mem_block = new (heap->mStart) MemBlock();

    mem_block->mSize = heap->mSize - sizeof(ExpHeap) - sizeof(MemBlock);
    mem_block->mHeapCheckTag = 0xFFFF;
    heap->mFreeList.pushBack(mem_block);
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

void ExpHeap::freeAll()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    dispose_(nullptr, nullptr);
    mUseList.clear();
    mFreeList.clear();

    createMaxSizeFreeMemBlock_(this);
}

void ExpHeap::free(void* ptr)
{
    freeAndGetAllocatableSize(ptr, sizeof(void*));
}

// Walks the blocks that physically follow 'block' until a free one is found
inline bool ExpHeap::findNeighborFreeMemBlocksByIterating_(MemBlock* block, MemBlock** prev,
                                                           MemBlock** next) const
{
    const void* area_end = PtrUtil::addOffset(
        mStart, static_cast<intptr_t>(
                    mDirection == cHeapDirection_Forward ? mSize : mSize - sizeof(ExpHeap)));

    MemBlock* it = block;
    do
    {
        it = reinterpret_cast<MemBlock*>(it->memory() + it->mSize);
        if (it >= area_end)
        {
            *next = it;
            return false;
        }
    } while (it->mHeapCheckTag != 0xFFFF);

    *next = it;
    *prev = mFreeList.prev(it);
    return true;
}

// Finds the first free block after 'block' ('next') and the free block before it ('prev').
// Returns false if there is no free block after 'block'.
inline bool ExpHeap::findNeighborFreeMemBlocks_(MemBlock* block, MemBlock** prev,
                                                MemBlock** next) const
{
    switch (mFindFreeBlockMode)
    {
    case FindFreeBlockMode::FromFreeList:
        break;
    case FindFreeBlockMode::ByIteratingMemBlock:
        if (mFreeList.size() > 1)
            return findNeighborFreeMemBlocksByIterating_(block, prev, next);
        break;
    default:
        if (mFreeList.size() * 3 > mUseList.size() + 3)
            return findNeighborFreeMemBlocksByIterating_(block, prev, next);
        break;
    }

    // The free list is sorted by address
    *prev = nullptr;
    for (auto& it : mFreeList)
    {
        if (&it > block)
        {
            *next = &it;
            return true;
        }
        *prev = &it;
    }
    return false;
}

MemBlock* ExpHeap::pushToFreeList_(MemBlock* block)
{
    MemBlock* prev;
    MemBlock* next;
    if (!findNeighborFreeMemBlocks_(block, &prev, &next))
    {
        // No free block after this one: merge into the last free block or append it
        if (mFreeList.size() != 0)
        {
            MemBlock* last = mFreeList.back();
            if (last->memory() + last->mSize == reinterpret_cast<u8*>(block))
            {
                last->mSize = last->mSize + block->mOffset + block->mSize + sizeof(MemBlock);
                return block;
            }
        }

        block->mHeapCheckTag = 0xFFFF;
        mFreeList.pushBack(block);
        return block;
    }

    // Merge with the previous free block if it ends where this one starts
    bool merged_with_prev = false;
    if (prev && prev->memory() + prev->mSize == reinterpret_cast<u8*>(block))
    {
        prev->mSize = prev->mSize + block->mOffset + block->mSize + sizeof(MemBlock);
        block = prev;
        merged_with_prev = true;
    }

    if (block->memory() + block->mSize != reinterpret_cast<u8*>(next))
    {
        if (!merged_with_prev)
        {
            block->mHeapCheckTag = 0xFFFF;
            mFreeList.insertBefore(next, block);
        }
        return block;
    }

    // Merge with the next free block, which starts where this one ends
    if (!merged_with_prev)
    {
        block->mHeapCheckTag = 0xFFFF;
        mFreeList.insertBefore(next, block);
    }
    block->mSize = block->mSize + next->mOffset + next->mSize + sizeof(MemBlock);
    mFreeList.erase(next);
    return block;
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

    return MemBlock::FindManageArea(object)->mSize;
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