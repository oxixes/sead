#include <heap/seadHeap.h>
#include <heap/seadHeapMgr.h>
#include <prim/seadScopedLock.h>

namespace sead
{
Heap::Heap(const SafeString& name, Heap* parent, void* address, size_t size,
           HeapDirection direction, bool enable_lock)
    : IDisposer(parent, HeapNullOption::UseSpecifiedOrContainHeap), INamable(name),
      hostio::Reflexible(), mStart(address), mSize(size), mParent(parent), mDirection(direction),
      mCS(parent)
{
    enableWarning(true);

    u32 tag;
    do
        tag = HeapMgr::sHeapCheckTag.increment();
    while ((tag & 0xFFFF) == 0xFFFF);
    mHeapCheckTag = static_cast<u16>(tag);

    enableLock(enable_lock);
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    mChildren.initOffset(offsetof(Heap, mListNode));
    mDisposerList.initOffset(offsetof(IDisposer, mListNode));
}

Heap::~Heap() = default;

void Heap::destruct_()
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    dispose_(nullptr, nullptr);
    HeapMgr::removeFromFindContainHeapCache_(this);
    if (mParent)
        mParent->eraseChild_(this);
    else
        HeapMgr::removeRootHeap(this);
}

void Heap::dispose_(const void* start, const void* end)
{
    enableDisposing(true);

    auto it = mDisposerList.begin();
    while (it != mDisposerList.end())
    {
        if (it->getDisposerHeap_() &&
            ((start == nullptr && end == nullptr) || PtrUtil::isInclude(&*it, start, end)))
        {
            it->~IDisposer();
            it = mDisposerList.begin();
        }
        else
        {
            ++it;
        }
    }

    enableDisposing(false);
}

void Heap::eraseChild_(Heap* child)
{
    auto mgrLock = makeScopedLock(HeapMgr::sHeapTreeLockCS);
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    mChildren.erase(child);
}

void Heap::appendDisposer_(IDisposer* disposer)
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    mDisposerList.pushBack(disposer);
}

void Heap::removeDisposer_(IDisposer* disposer)
{
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());
    mDisposerList.erase(disposer);
}

Heap* Heap::findContainHeap_(const void* ptr)
{
    if (!isInclude(ptr))
        return nullptr;

    for (auto it = mChildren.begin(); it != mChildren.end(); ++it)
    {
        if (it->isInclude(ptr))
            return it->findContainHeap_(ptr);
    }

    return this;
}

void Heap::pushBackChild_(Heap* child)
{
    auto mgrLock = makeScopedLock(HeapMgr::sHeapTreeLockCS);
    ConditionalScopedLock<CriticalSection> lock(&mCS, isLockEnabled());

    mChildren.pushBack(child);
}


void Heap::genInformation_(hostio::Context*) {}

void Heap::makeMetaString_(BufferedSafeString* str)
{
    s32 used_size_tenths = static_cast<s32>(
        (1.0f - (static_cast<f32>(getFreeSize()) / static_cast<f32>(getSize()))) * 10.0f);
    str->format("$SEAD_META_HEAP_%03d", used_size_tenths * 10);
}

}  // namespace sead
