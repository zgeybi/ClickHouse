#include <Interpreters/Cache/LRUFileCachePriority.h>
#include <Interpreters/Cache/FileCache.h>
#include <Interpreters/Cache/EvictionCandidates.h>
#include <Common/CurrentMetrics.h>
#include <Common/randomSeed.h>
#include <Common/logger_useful.h>
#include <pcg-random/pcg_random.hpp>

namespace CurrentMetrics
{
    extern const Metric FilesystemCacheSize;
    extern const Metric FilesystemCacheElements;
}

namespace ProfileEvents
{
    extern const Event FilesystemCacheEvictionSkippedFileSegments;
    extern const Event FilesystemCacheEvictionTries;
    extern const Event FilesystemCacheEvictMicroseconds;
    extern const Event FilesystemCacheEvictedBytes;
    extern const Event FilesystemCacheEvictedFileSegments;
    extern const Event FilesystemCacheEvictionSkippedEvictingFileSegments;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

LRUFileCachePriority::LRUFileCachePriority(
    size_t max_size_,
    size_t max_elements_,
    StatePtr state_,
    const std::string & description_)
    : IFileCachePriority(max_size_, max_elements_)
    , description(description_)
    , log(getLogger("LRUFileCachePriority{}" + (description.empty() ? "" : "(" + description + ")")))
{
    if (state_)
        state = state_;
    else
        state = std::make_shared<State>();
}

IFileCachePriority::IteratorPtr LRUFileCachePriority::add( /// NOLINT
    KeyMetadataPtr key_metadata,
    size_t offset,
    size_t size,
    const UserInfo &,
    const CachePriorityGuard::Lock & lock,
    bool)
{
    return std::make_shared<LRUIterator>(add(std::make_shared<Entry>(key_metadata->key, offset, size, key_metadata), lock));
}

LRUFileCachePriority::LRUIterator LRUFileCachePriority::add(EntryPtr entry, const CachePriorityGuard::Lock & lock)
{
    if (entry->size == 0)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Adding zero size entries to LRU queue is not allowed "
            "(key: {}, offset: {})", entry->key, entry->offset);
    }

#ifndef NDEBUG
    for (const auto & queue_entry : queue)
    {
        /// entry.size == 0 means entry was invalidated.
        if (queue_entry->size != 0 && queue_entry->key == entry->key && queue_entry->offset == entry->offset)
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Attempt to add duplicate queue entry to queue: {}",
                entry->toString());
        }
    }
#endif

    if (!canFit(entry->size, 1, lock))
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Not enough space to add a new entry {}. Current state: {}",
            entry->toString(), getStateInfoForLog(lock));
    }

    auto iterator = queue.insert(queue.end(), entry);

    updateSize(entry->size);
    updateElementsCount(1);

    LOG_TEST(
        log, "Added entry into LRU queue, key: {}, offset: {}, size: {}",
        entry->key, entry->offset, entry->size);

    return LRUIterator(this, iterator);
}

LRUFileCachePriority::LRUQueue::iterator
LRUFileCachePriority::remove(LRUQueue::iterator it, const CachePriorityGuard::Lock &)
{
    /// If size is 0, entry is invalidated, current_elements_num was already updated.
    const auto & entry = **it;
    if (entry.size)
    {
        updateSize(-entry.size);
        updateElementsCount(-1);
    }

    LOG_TEST(
        log, "Removed entry from LRU queue, key: {}, offset: {}, size: {}",
        entry.key, entry.offset, entry.size);

    return queue.erase(it);
}

void LRUFileCachePriority::updateSize(int64_t size)
{
    chassert(size != 0);
    chassert(size > 0 || state->current_size >= size_t(-size));

    state->current_size += size;
    CurrentMetrics::add(CurrentMetrics::FilesystemCacheSize, size);
}

void LRUFileCachePriority::updateElementsCount(int64_t num)
{
    state->current_elements_num += num;
    CurrentMetrics::add(CurrentMetrics::FilesystemCacheElements, num);
}

LRUFileCachePriority::LRUIterator::LRUIterator(
    LRUFileCachePriority * cache_priority_,
    LRUQueue::iterator iterator_)
    : cache_priority(cache_priority_)
    , iterator(iterator_)
{
}

LRUFileCachePriority::LRUIterator::LRUIterator(const LRUIterator & other)
{
    *this = other;
}

LRUFileCachePriority::LRUIterator & LRUFileCachePriority::LRUIterator::operator =(const LRUIterator & other)
{
    if (this == &other)
        return *this;

    cache_priority = other.cache_priority;
    iterator = other.iterator;
    return *this;
}

bool LRUFileCachePriority::LRUIterator::operator ==(const LRUIterator & other) const
{
    return cache_priority == other.cache_priority && iterator == other.iterator;
}

void LRUFileCachePriority::iterate(IterateFunc && func, const CachePriorityGuard::Lock & lock)
{
    for (auto it = queue.begin(); it != queue.end();)
    {
        const auto & entry = **it;

        if (entry.size == 0)
        {
            it = remove(it, lock);
            continue;
        }

        if (entry.isEvicting(lock))
        {
            ++it;
            ProfileEvents::increment(ProfileEvents::FilesystemCacheEvictionSkippedEvictingFileSegments);
            continue;
        }

        auto locked_key = entry.key_metadata->tryLock();
        if (!locked_key || entry.size == 0)
        {
            it = remove(it, lock);
            continue;
        }

        auto metadata = locked_key->tryGetByOffset(entry.offset);
        if (!metadata)
        {
            it = remove(it, lock);
            continue;
        }

        if (metadata->size() != entry.size)
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Mismatch of file segment size in file segment metadata "
                "and priority queue: {} != {} ({})",
                entry.size, metadata->size(), metadata->file_segment->getInfoForLog());
        }

        auto result = func(*locked_key, metadata);
        switch (result)
        {
            case IterationResult::BREAK:
            {
                return;
            }
            case IterationResult::CONTINUE:
            {
                ++it;
                break;
            }
            case IterationResult::REMOVE_AND_CONTINUE:
            {
                it = remove(it, lock);
                break;
            }
        }
    }
}

bool LRUFileCachePriority::canFit( /// NOLINT
    size_t size,
    size_t elements,
    const CachePriorityGuard::Lock & lock,
    IteratorPtr,
    bool) const
{
    return canFit(size, elements, 0, 0, nullptr, nullptr, lock);
}

bool LRUFileCachePriority::canFit(
    size_t size,
    size_t elements,
    size_t released_size_assumption,
    size_t released_elements_assumption,
    bool * reached_size_limit,
    bool * reached_elements_limit,
    const CachePriorityGuard::Lock &) const
{
    const bool size_limit_satisifed = max_size == 0 || state->current_size + size - released_size_assumption <= max_size;
    const bool elements_limit_satisfied = max_elements == 0 || state->current_elements_num + elements - released_elements_assumption <= max_elements;

    if (reached_size_limit)
        *reached_size_limit |= !size_limit_satisifed;
    if (reached_elements_limit)
        *reached_elements_limit |= !elements_limit_satisfied;

    return size_limit_satisifed && elements_limit_satisfied;
}

bool LRUFileCachePriority::collectCandidatesForEviction(
    size_t size,
    FileCacheReserveStat & stat,
    EvictionCandidates & res,
    IFileCachePriority::IteratorPtr,
    const UserID &,
    bool & reached_size_limit,
    bool & reached_elements_limit,
    const CachePriorityGuard::Lock & lock)
{
    if (canFit(size, 1, 0, 0, &reached_size_limit, &reached_elements_limit, lock))
        return true;

    ProfileEvents::increment(ProfileEvents::FilesystemCacheEvictionTries);

    IterateFunc iterate_func = [&](LockedKey & locked_key, const FileSegmentMetadataPtr & segment_metadata)
    {
        const auto & file_segment = segment_metadata->file_segment;
        chassert(file_segment->assertCorrectness());

        if (segment_metadata->releasable())
        {
            res.add(segment_metadata, locked_key, lock);
            stat.update(segment_metadata->size(), file_segment->getKind(), true);
        }
        else
        {
            stat.update(segment_metadata->size(), file_segment->getKind(), false);
            ProfileEvents::increment(ProfileEvents::FilesystemCacheEvictionSkippedFileSegments);
        }

        return IterationResult::CONTINUE;
    };

    auto can_fit = [&]
    {
        return canFit(size, 1, stat.stat.releasable_size, stat.stat.releasable_count, nullptr, nullptr, lock);
    };

    iterate([&](LockedKey & locked_key, const FileSegmentMetadataPtr & segment_metadata)
    {
        return can_fit() ? IterationResult::BREAK : iterate_func(locked_key, segment_metadata);
    }, lock);

    return can_fit();
}

LRUFileCachePriority::LRUIterator
LRUFileCachePriority::move(LRUIterator & it, LRUFileCachePriority & other, const CachePriorityGuard::Lock &)
{
    const auto & entry = *it.getEntry();
    if (entry.size == 0)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "Adding zero size entries to LRU queue is not allowed "
            "(key: {}, offset: {})", entry.key, entry.offset);
    }
#ifndef NDEBUG
    for (const auto & queue_entry : queue)
    {
        /// entry.size == 0 means entry was invalidated.
        if (queue_entry->size != 0 && queue_entry->key == entry.key && queue_entry->offset == entry.offset)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Attempt to add duplicate queue entry to queue: {}",
                entry.toString());
    }
#endif

    queue.splice(queue.end(), other.queue, it.iterator);

    updateSize(entry.size);
    updateElementsCount(1);

    other.updateSize(-entry.size);
    other.updateElementsCount(-1);
    return LRUIterator(this, it.iterator);
}

IFileCachePriority::PriorityDumpPtr LRUFileCachePriority::dump(const CachePriorityGuard::Lock & lock)
{
    std::vector<FileSegmentInfo> res;
    iterate([&](LockedKey &, const FileSegmentMetadataPtr & segment_metadata)
    {
        res.emplace_back(FileSegment::getInfo(segment_metadata->file_segment));
        return IterationResult::CONTINUE;
    }, lock);
    return std::make_shared<LRUPriorityDump>(res);
}

bool LRUFileCachePriority::modifySizeLimits(
    size_t max_size_, size_t max_elements_, double /* size_ratio_ */, const CachePriorityGuard::Lock & lock)
{
    if (max_size == max_size_ && max_elements == max_elements_)
        return false; /// Nothing to change.

    auto check_limits_satisfied = [&]()
    {
        return (max_size_ == 0 || state->current_size <= max_size_)
            && (max_elements_ == 0 || state->current_elements_num <= max_elements_);
    };

    if (check_limits_satisfied())
    {
        max_size = max_size_;
        max_elements = max_elements_;
        return true;
    }

    auto iterate_func = [&](LockedKey & locked_key, const FileSegmentMetadataPtr & segment_metadata)
    {
        chassert(segment_metadata->file_segment->assertCorrectness());

        if (!segment_metadata->releasable())
            return IterationResult::CONTINUE;

        auto segment = segment_metadata->file_segment;
        locked_key.removeFileSegment(segment->offset(), segment->lock());

        ProfileEvents::increment(ProfileEvents::FilesystemCacheEvictedFileSegments);
        ProfileEvents::increment(ProfileEvents::FilesystemCacheEvictedBytes, segment->getDownloadedSize());
        return IterationResult::REMOVE_AND_CONTINUE;
    };

    auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::FilesystemCacheEvictMicroseconds);
    iterate(
        [&](LockedKey & locked_key, const FileSegmentMetadataPtr & segment_metadata)
        { return check_limits_satisfied() ? IterationResult::BREAK : iterate_func(locked_key, segment_metadata); },
        lock);

    max_size = max_size_;
    max_elements = max_elements_;
    return true;
}

void LRUFileCachePriority::LRUIterator::remove(const CachePriorityGuard::Lock & lock)
{
    assertValid();
    cache_priority->remove(iterator, lock);
    iterator = LRUQueue::iterator{};
}

void LRUFileCachePriority::LRUIterator::invalidate()
{
    assertValid();

    const auto & entry = *iterator;
    LOG_TEST(cache_priority->log,
             "Invalidating entry in LRU queue entry {}", entry->toString());

    chassert(entry->size != 0);
    cache_priority->updateSize(-entry->size);
    cache_priority->updateElementsCount(-1);
    entry->size = 0;
}

void LRUFileCachePriority::LRUIterator::incrementSize(size_t size, const CachePriorityGuard::Lock & lock)
{
    chassert(size);
    assertValid();

    const auto & entry = *iterator;

    if (!cache_priority->canFit(size, /* elements */0, lock))
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Cannot increment size by {} for entry {}. Current state: {}",
                        size, entry->toString(), cache_priority->getStateInfoForLog(lock));
    }

    LOG_TEST(
        cache_priority->log,
        "Incrementing size with {} in LRU queue for entry {}",
        size, entry->toString());

    cache_priority->updateSize(size);
    entry->size += size;

    cache_priority->check(lock);
}

void LRUFileCachePriority::LRUIterator::decrementSize(size_t size)
{
    assertValid();

    const auto & entry = *iterator;
    LOG_TEST(cache_priority->log,
             "Decrement size with {} in LRU queue entry {}",
             size, entry->toString());

    chassert(size);
    chassert(entry->size >= size);

    cache_priority->updateSize(-size);
    entry->size -= size;
}

size_t LRUFileCachePriority::LRUIterator::increasePriority(const CachePriorityGuard::Lock & lock)
{
    assertValid();
    cache_priority->queue.splice(cache_priority->queue.end(), cache_priority->queue, iterator);
    cache_priority->check(lock);
    return ++((*iterator)->hits);
}

void LRUFileCachePriority::LRUIterator::assertValid() const
{
    if (iterator == LRUQueue::iterator{})
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Attempt to use invalid iterator");
}

void LRUFileCachePriority::shuffle(const CachePriorityGuard::Lock &)
{
    std::vector<LRUQueue::iterator> its;
    its.reserve(queue.size());
    for (auto it = queue.begin(); it != queue.end(); ++it)
        its.push_back(it);
    pcg64 generator(randomSeed());
    std::shuffle(its.begin(), its.end(), generator);
    for (auto & it : its)
        queue.splice(queue.end(), queue, it);
}

std::string LRUFileCachePriority::getStateInfoForLog(const CachePriorityGuard::Lock & lock) const
{
    return fmt::format("size: {}/{}, elements: {}/{} (description: {})",
                       getSize(lock), max_size, getElementsCount(lock), max_elements, description);
}

void LRUFileCachePriority::holdImpl(
    size_t size,
    size_t elements,
    QueueEntryType /* queue_entry_type */,
    const CachePriorityGuard::Lock & lock)
{
    if (!canFit(size, elements, lock))
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "Cannot take space {} in size and {} in elements. "
                        "Current state {}/{} in size, {}/{} in elements",
                        size, elements, state->current_size, max_size,
                        state->current_elements_num, max_elements);
    }

    state->current_size += size;
    state->current_elements_num += elements;
}

void LRUFileCachePriority::releaseImpl(size_t size, size_t elements, QueueEntryType /* queue_entry_type */)
{
    state->current_size -= size;
    state->current_elements_num -= elements;
}

}
