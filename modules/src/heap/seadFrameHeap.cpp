#include <basis/seadNew.h>
#include <heap/seadFrameHeap.h>
#include <heap/seadHeapMgr.h>
#include <math/seadMathCalcCommon.hpp>
#include <prim/seadPtrUtil.h>
#include <prim/seadScopedLock.h>

namespace sead
{
FrameHeap::FrameHeap(const SafeString& name, Heap* parent, void* address, size_t size,
                     HeapDirection direction, bool enable_lock)
    : Heap(name, parent, address, size, direction, enable_lock)
{
    mState.mHeadPtr = nullptr;
    mState.mTailPtr = nullptr;
}

FrameHeap::~FrameHeap()
{
    destruct_();
}

FrameHeap* FrameHeap::tryCreate(size_t size, const SafeString& name, Heap* parent, s32 alignment,
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

    if (size < sizeof(FrameHeap))
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

    heap->initialize_();

    parent->pushBackChild_(heap);
    return heap;
}

FrameHeap* FrameHeap::create(size_t size, const SafeString& name, Heap* parent, s32 alignment,
                             HeapDirection direction, bool enable_lock)
{
    FrameHeap* heap = tryCreate(size, name, parent, alignment, direction, enable_lock);
    SEAD_ASSERT(heap);
    return heap;
}

void FrameHeap::freeTail()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    if (mDirection == cHeapDirection_Forward)
    {
        dispose_(mState.mTailPtr, PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize)));
        mState.mTailPtr = getAreaEnd_();
    }
    else
    {
        dispose_(mStart, mState.mHeadPtr);
        mState.mHeadPtr = getAreaStart_();
    }
}

void FrameHeap::destroy()
{
    Heap* parent = mParent;
    void* start = mStart;
    this->~FrameHeap();
    if (parent && parent->isFreeable())
        parent->free(start);
}

void FrameHeap::free(void*) {}

void* FrameHeap::resizeFront(void*, size_t)
{
    return nullptr;
}

void* FrameHeap::resizeBack(void*, size_t)
{
    return nullptr;
}

void FrameHeap::freeAll()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    dispose_(nullptr, nullptr);
    mState.mHeadPtr = getAreaStart_();
    mState.mTailPtr = getAreaEnd_();
}

uintptr_t FrameHeap::getStartAddress() const
{
    return reinterpret_cast<uintptr_t>(mStart);
}

uintptr_t FrameHeap::getEndAddress() const
{
    return reinterpret_cast<uintptr_t>(mStart) + mSize;
}

size_t FrameHeap::getSize() const
{
    return mSize;
}

size_t FrameHeap::getFreeSize() const
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    return PtrUtil::diff(mState.mTailPtr, mState.mHeadPtr);
}

bool FrameHeap::isInclude(const void* p_void) const
{
    return PtrUtil::isInclude(p_void, getAreaStart_(), getAreaEnd_());
}

bool FrameHeap::isEmpty() const
{
    return mState.mHeadPtr == getAreaStart_() && mState.mTailPtr == getAreaEnd_();
}

bool FrameHeap::isFreeable() const
{
    return false;
}

bool FrameHeap::isResizable() const
{
    return false;
}

bool FrameHeap::isAdjustable() const
{
    return true;
}

void FrameHeap::dump() const
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    // The output is stripped from release builds and only the lock remains. A debug build would
    // have to output something
}

void FrameHeap::genInformation_(hostio::Context* context)
{
    Heap::genInformation_(context);
}

void FrameHeap::initialize_()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    mState.mHeadPtr = getAreaStart_();
    mState.mTailPtr = getAreaEnd_();
}

void* FrameHeap::getAreaStart_() const
{
    if (mDirection == cHeapDirection_Forward)
    {
        return PtrUtil::addOffset(mStart, sizeof(FrameHeap));
    }

    return mStart;
}

void* FrameHeap::getAreaEnd_() const
{
    if (mDirection == cHeapDirection_Forward)
        return PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize));

    return PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize - sizeof(FrameHeap)));
}

}  // namespace sead
