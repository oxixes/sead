#include <atomic>
#include <basis/seadNew.h>
#include <heap/seadFrameHeap.h>
#include <heap/seadHeapMgr.h>
#include <math/seadMathCalcCommon.hpp>
#include <prim/seadPtrUtil.h>
#include <prim/seadScopedLock.h>
#include <stream/seadStream.h>

namespace sead
{
namespace
{
inline void notifyAllocFailed(HeapMgr* heap_mgr, Heap* heap, size_t request_size,
                              s32 request_alignment, size_t alloc_size, s32 alloc_alignment)
{
    if (!heap_mgr)
        return;

    HeapMgr::IAllocFailedCallback* callback = heap_mgr->getAllocFailedCallback();
    if (!callback)
        return;

    HeapMgr::AllocFailedCallbackArg arg;
    arg.heap = heap;
    arg.request_size = request_size;
    arg.request_alignment = request_alignment;
    arg.alloc_size = alloc_size;
    arg.alloc_alignment = alloc_alignment;
    callback->invoke(&arg);
}
}  // namespace

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

size_t FrameHeap::getManagementAreaSize(s32 alignment)
{
    return alignment + sizeof(FrameHeap);
}

void FrameHeap::restoreState(const State& state)
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    if (state.mHeadPtr != nullptr && state.mHeadPtr != mState.mHeadPtr &&
        isInclude(state.mHeadPtr) && state.mHeadPtr <= mState.mHeadPtr)
    {
        dispose_(state.mHeadPtr, mState.mHeadPtr);
        mState.mHeadPtr = state.mHeadPtr;
    }

    if (state.mTailPtr != nullptr && state.mTailPtr != mState.mTailPtr &&
        isInclude(state.mTailPtr) && state.mTailPtr >= mState.mTailPtr)
    {
        dispose_(mState.mTailPtr, state.mTailPtr);
        mState.mTailPtr = state.mTailPtr;
    }
}

void FrameHeap::freeHead()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    if (mDirection == cHeapDirection_Forward)
    {
        dispose_(PtrUtil::addOffset(mStart, sizeof(FrameHeap)), mState.mHeadPtr);
        mState.mHeadPtr = getAreaStart_();
    }
    else
    {
        dispose_(mState.mTailPtr, PtrUtil::addOffset(mStart, static_cast<intptr_t>(mSize - sizeof(FrameHeap))));
        mState.mTailPtr = getAreaEnd_();
    }
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

size_t FrameHeap::adjust()
{
    if (!mParent)
        return mSize;

    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    ScopedLock<Heap> parent_lock(mParent);

    if (mDirection == cHeapDirection_Forward)
    {
        if (mState.mTailPtr == getAreaEnd_())
            return adjustBack_();
        return mSize;
    }

    if (mState.mHeadPtr == getAreaStart_())
        return adjustFront_();
    return mSize;
}

void* FrameHeap::tryAlloc(size_t size, s32 alignment)
{
    HeapMgr* heap_mgr = HeapMgr::instance();
    size_t alloc_size = MathSizeT::roundUp(MathSizeT::max(size, sizeof(void*)), sizeof(void*));

    if ((Mathi::abs(alignment) - 1) & Mathi::abs(alignment))
    {
        notifyAllocFailed(heap_mgr, this, size, alignment, alloc_size, alignment);
        return nullptr;
    }

    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    s32 directed_alignment = mDirection * alignment;
    if (directed_alignment >= 0)
    {
        void* ptr = PtrUtil::roundUpPow2(mState.mHeadPtr, directed_alignment);
        void* new_head = PtrUtil::addOffset(ptr, static_cast<intptr_t>(alloc_size));
        if (new_head < ptr)
        {
            notifyAllocFailed(heap_mgr, this, size, alignment, alloc_size, directed_alignment);
            return nullptr;
        }

        if (new_head > mState.mTailPtr)
        {
            notifyAllocFailed(heap_mgr, this, size, alignment, alloc_size, directed_alignment);
            return nullptr;
        }

        mState.mHeadPtr = new_head;
        return ptr;
    }

    void* end = PtrUtil::addOffset(mState.mTailPtr, -static_cast<intptr_t>(alloc_size));
    if (end > mState.mTailPtr)
    {
        notifyAllocFailed(heap_mgr, this, size, alignment, alloc_size, -directed_alignment);
        return nullptr;
    }

    void* ptr = PtrUtil::roundDownPow2(end, -directed_alignment);
    if (ptr < mState.mHeadPtr)
    {
        notifyAllocFailed(heap_mgr, this, size, alignment, alloc_size, -directed_alignment);
        return nullptr;
    }

    mState.mTailPtr = ptr;
    return ptr;
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

size_t FrameHeap::getMaxAllocatableSize(int alignment) const
{
    if ((Mathi::abs(alignment) - 1) & Mathi::abs(alignment))
        return 0;

    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    void* alignedHead = PtrUtil::roundUpPow2(mState.mHeadPtr, Mathi::abs(alignment));
    if (alignedHead <= mState.mTailPtr)
        return PtrUtil::diff(mState.mTailPtr, alignedHead);

    return 0;
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

void FrameHeap::dumpYAML(WriteStream& stream, int i) const
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    Heap::dumpYAML(stream, i);

    FixedSafeString<128> str("");
    str.append(' ', i);
    str.appendWithFormat("  heap_type: FrameHeap\n");
    stream.writeDecorationText(str);

    str.clear();
    str.append(' ', i);
    str.appendWithFormat("  head_ptr: 0x%016llX\n", mState.mHeadPtr);
    stream.writeDecorationText(str);

    str.clear();
    str.append(' ', i);
    str.appendWithFormat("  tail_ptr: 0x%016llX\n", mState.mTailPtr);
    stream.writeDecorationText(str);
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

size_t FrameHeap::adjustFront_()
{
    size_t newSize = PtrUtil::diff(reinterpret_cast<void*>(getEndAddress()), mState.mTailPtr);
    if (mParent->resizeFront(mStart, newSize) == nullptr)
        return mSize;

    mSize = newSize;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    mState.mHeadPtr = mState.mTailPtr;
    mStart = mState.mTailPtr;

    return newSize;
}

size_t FrameHeap::adjustBack_()
{
    size_t newSize = PtrUtil::diff(mState.mHeadPtr, reinterpret_cast<void*>(getStartAddress()));
    if (mParent->resizeBack(mStart, newSize) == nullptr)
        return mSize;

    mState.mTailPtr = mState.mHeadPtr;
    mSize = newSize;

    return newSize;
}

}  // namespace sead
