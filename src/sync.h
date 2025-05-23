// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SYNC_H
#define BITCOIN_SYNC_H

#ifdef DEBUG_LOCKCONTENTION
#include <logging.h>
#include <logging/timer.h>
#endif

#include <threadsafety.h>
#include <util/macros.h>

#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>

/////////////////////////////////////////////////
//                                             //
// THE SIMPLE DEFINITION, EXCLUDING DEBUG CODE //
//                                             //
/////////////////////////////////////////////////

/*
RecursiveMutex mutex;
    std::recursive_mutex mutex;

LOCK(mutex);
    std::unique_lock<std::recursive_mutex> criticalblock(mutex);

LOCK2(mutex1, mutex2);
    std::unique_lock<std::recursive_mutex> criticalblock1(mutex1);
    std::unique_lock<std::recursive_mutex> criticalblock2(mutex2);

TRY_LOCK(mutex, name);
    std::unique_lock<std::recursive_mutex> name(mutex, std::try_to_lock_t);

ENTER_CRITICAL_SECTION(mutex); // no RAII
    mutex.lock();

LEAVE_CRITICAL_SECTION(mutex); // no RAII
    mutex.unlock();
 */

///////////////////////////////
//                           //
// THE ACTUAL IMPLEMENTATION //
//                           //
///////////////////////////////

#ifdef DEBUG_LOCKORDER
template <typename MutexType>
void EnterCritical(const char* pszName, const char* pszFile, int nLine, MutexType* cs, bool fTry = false);
void LeaveCritical();
void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line);
std::string LocksHeld();
template <typename MutexType>
void AssertLockHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) EXCLUSIVE_LOCKS_REQUIRED(cs);
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs);
void DeleteLock(void* cs);
bool LockStackEmpty();

/**
 * Call abort() if a potential lock order deadlock bug is detected, instead of
 * just logging information and throwing a logic_error. Defaults to true, and
 * set to false in DEBUG_LOCKORDER unit tests.
 */
extern bool g_debug_lockorder_abort;
#else
template <typename MutexType>
inline void EnterCritical(const char* pszName, const char* pszFile, int nLine, MutexType* cs, bool fTry = false) {}
inline void LeaveCritical() {}
inline void CheckLastCritical(void* cs, std::string& lockname, const char* guardname, const char* file, int line) {}
template <typename MutexType>
inline void AssertLockHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) EXCLUSIVE_LOCKS_REQUIRED(cs) {}
template <typename MutexType>
void AssertLockNotHeldInternal(const char* pszName, const char* pszFile, int nLine, MutexType* cs) LOCKS_EXCLUDED(cs) {}
inline void DeleteLock(void* cs) {}
inline bool LockStackEmpty() { return true; }
#endif

/**
 * Template mixin that adds -Wthread-safety locking annotations and lock order
 * checking to a subset of the mutex API.
 */
template <typename PARENT>
class LOCKABLE AnnotatedMixin : public PARENT
{
public:
    ~AnnotatedMixin() {
        DeleteLock((void*)this);
    }

    void lock() EXCLUSIVE_LOCK_FUNCTION()
    {
        PARENT::lock();
    }

    void unlock() UNLOCK_FUNCTION()
    {
        PARENT::unlock();
    }

    bool try_lock() EXCLUSIVE_TRYLOCK_FUNCTION(true)
    {
        return PARENT::try_lock();
    }

    using UniqueLock = std::unique_lock<PARENT>;
#ifdef __clang__
    //! For negative capabilities in the Clang Thread Safety Analysis.
    //! A negative requirement uses the EXCLUSIVE_LOCKS_REQUIRED attribute, in conjunction
    //! with the ! operator, to indicate that a mutex should not be held.
    const AnnotatedMixin& operator!() const { return *this; }
#endif // __clang__
};

template <typename PARENT>
class LOCKABLE SharedAnnotatedMixin : public AnnotatedMixin<PARENT>
{
public:
    bool try_shared_lock() SHARED_TRYLOCK_FUNCTION(true)
    {
        return PARENT::try_shared_lock();
    }
    void shared_lock() SHARED_LOCK_FUNCTION()
    {
        PARENT::shared_lock();
    }
    using SharedLock = std::shared_lock<PARENT>;
};

/**
 * Wrapped mutex: supports recursive locking, but no waiting
 * TODO: We should move away from using the recursive lock by default.
 */
using RecursiveMutex = AnnotatedMixin<std::recursive_mutex>;

/** Wrapped mutex: supports waiting but not recursive locking */
using Mutex = AnnotatedMixin<std::mutex>;

/** Wrapped shared mutex: supports read locking via .shared_lock, exlusive locking via .lock;
 * does not support recursive locking */
using SharedMutex = SharedAnnotatedMixin<std::shared_mutex>;

#define AssertLockHeld(cs) AssertLockHeldInternal(#cs, __FILE__, __LINE__, &cs)

inline void AssertLockNotHeldInline(const char* name, const char* file, int line, Mutex* cs) EXCLUSIVE_LOCKS_REQUIRED(!cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, RecursiveMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
inline void AssertLockNotHeldInline(const char* name, const char* file, int line, SharedMutex* cs) LOCKS_EXCLUDED(cs) { AssertLockNotHeldInternal(name, file, line, cs); }
#define AssertLockNotHeld(cs) AssertLockNotHeldInline(#cs, __FILE__, __LINE__, &cs)

/** Wrapper around std::unique_lock style lock for Mutex. */
template <typename Mutex, typename Base = typename Mutex::UniqueLock>
class SCOPED_LOCKABLE UniqueLock : public Base
{
private:
    void Enter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex());
#ifdef DEBUG_LOCKCONTENTION
        if (Base::try_lock()) return;
        LOG_TIME_MICROS_WITH_CATEGORY(strprintf("lock contention %s, %s:%d", pszName, pszFile, nLine), BCLog::LOCK);
#endif
        Base::lock();
    }

    bool TryEnter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex(), true);
        if (Base::try_lock()) {
            return true;
        }
        LeaveCritical();
        return false;
    }

public:
    UniqueLock(Mutex& mutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(mutexIn) : Base(mutexIn, std::defer_lock)
    {
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    UniqueLock(Mutex* pmutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) EXCLUSIVE_LOCK_FUNCTION(pmutexIn)
    {
        if (!pmutexIn) return;

        *static_cast<Base*>(this) = Base(*pmutexIn, std::defer_lock);
        if (fTry)
            TryEnter(pszName, pszFile, nLine);
        else
            Enter(pszName, pszFile, nLine);
    }

    ~UniqueLock() UNLOCK_FUNCTION()
    {
        if (Base::owns_lock())
            LeaveCritical();
    }

    operator bool()
    {
        return Base::owns_lock();
    }

protected:
    // needed for reverse_lock
    UniqueLock() { }

public:
    /**
     * An RAII-style reverse lock. Unlocks on construction and locks on destruction.
     */
    class reverse_lock {
    public:
        explicit reverse_lock(UniqueLock& _lock, const char* _guardname, const char* _file, int _line) : lock(_lock), file(_file), line(_line) {
            CheckLastCritical((void*)lock.mutex(), lockname, _guardname, _file, _line);
            lock.unlock();
            LeaveCritical();
            lock.swap(templock);
        }

        ~reverse_lock() {
            templock.swap(lock);
            EnterCritical(lockname.c_str(), file.c_str(), line, lock.mutex());
            lock.lock();
        }

     private:
        reverse_lock(reverse_lock const&);
        reverse_lock& operator=(reverse_lock const&);

        UniqueLock& lock;
        UniqueLock templock;
        std::string lockname;
        const std::string file;
        const int line;
     };
     friend class reverse_lock;
};

template <typename Mutex, typename Base = typename Mutex::SharedLock>
class SCOPED_LOCKABLE SharedLock : public Base
{
private:
    void SharedEnter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex());
#ifdef DEBUG_LOCKCONTENTION
        if (Base::try_lock()) return;
        LOG_TIME_MICROS_WITH_CATEGORY(strprintf("lock contention %s, %s:%d", pszName, pszFile, nLine), BCLog::LOCK);
#endif
        Base::lock();
    }

    bool TrySharedEnter(const char* pszName, const char* pszFile, int nLine)
    {
        EnterCritical(pszName, pszFile, nLine, Base::mutex(), true);
        if (Base::try_lock()) {
            return true;
        }
        LeaveCritical();
        return false;
    }

public:
    SharedLock(Mutex& mutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) SHARED_LOCK_FUNCTION(mutexIn) : Base(mutexIn, std::defer_lock)
    {
        if (fTry) {
            TrySharedEnter(pszName, pszFile, nLine);
        } else {
            SharedEnter(pszName, pszFile, nLine);
        }
    }

    SharedLock(Mutex* pmutexIn, const char* pszName, const char* pszFile, int nLine, bool fTry = false) SHARED_LOCK_FUNCTION(pmutexIn)
    {
        if (!pmutexIn) return;

        *static_cast<Base*>(this) = Base(*pmutexIn, std::defer_lock);
        if (fTry) {
            TrySharedEnter(pszName, pszFile, nLine);
        } else {
            SharedEnter(pszName, pszFile, nLine);
        }
    }

    ~SharedLock() UNLOCK_FUNCTION()
    {
        if (Base::owns_lock()) {
            LeaveCritical();
        }
    }
};

#define REVERSE_LOCK(g) typename std::decay<decltype(g)>::type::reverse_lock UNIQUE_NAME(revlock)(g, #g, __FILE__, __LINE__)

template<typename MutexArg>
using DebugLock = UniqueLock<typename std::remove_reference<typename std::remove_pointer<MutexArg>::type>::type>;
template<typename MutexArg>
using ReadLock = SharedLock<typename std::remove_reference<typename std::remove_pointer<MutexArg>::type>::type>;

#define LOCK(cs) DebugLock<decltype(cs)> UNIQUE_NAME(criticalblock)(cs, #cs, __FILE__, __LINE__)
#define READ_LOCK(cs) ReadLock<decltype(cs)> UNIQUE_NAME(criticalblock)(cs, #cs, __FILE__, __LINE__)
#define LOCK2(cs1, cs2)                                               \
    DebugLock<decltype(cs1)> criticalblock1(cs1, #cs1, __FILE__, __LINE__); \
    DebugLock<decltype(cs2)> criticalblock2(cs2, #cs2, __FILE__, __LINE__)
#define TRY_LOCK(cs, name) DebugLock<decltype(cs)> name(cs, #cs, __FILE__, __LINE__, true)
#define TRY_READ_LOCK(cs, name) ReadLock<decltype(cs)> name(cs, #cs, __FILE__, __LINE__, true)
#define WAIT_LOCK(cs, name) DebugLock<decltype(cs)> name(cs, #cs, __FILE__, __LINE__)

#define ENTER_CRITICAL_SECTION(cs)                            \
    {                                                         \
        EnterCritical(#cs, __FILE__, __LINE__, &cs); \
        (cs).lock();                                          \
    }

#define LEAVE_CRITICAL_SECTION(cs)                                          \
    {                                                                       \
        std::string lockname;                                               \
        CheckLastCritical(reinterpret_cast<void*>(&cs), lockname, #cs, __FILE__, __LINE__); \
        (cs).unlock();                                                      \
        LeaveCritical();                                                    \
    }

//! Run code while locking a mutex.
//!
//! Examples:
//!
//!   WITH_LOCK(cs, shared_val = shared_val + 1);
//!
//!   int val = WITH_LOCK(cs, return shared_val);
//!
//! Note:
//!
//! Since the return type deduction follows that of decltype(auto), while the
//! deduced type of:
//!
//!   WITH_LOCK(cs, return {int i = 1; return i;});
//!
//! is int, the deduced type of:
//!
//!   WITH_LOCK(cs, return {int j = 1; return (j);});
//!
//! is &int, a reference to a local variable
//!
//! The above is detectable at compile-time with the -Wreturn-local-addr flag in
//! gcc and the -Wreturn-stack-address flag in clang, both enabled by default.
#define WITH_LOCK(cs, code) [&]() -> decltype(auto) { LOCK(cs); code; }()
#define WITH_READ_LOCK(cs, code) [&]() -> decltype(auto) { READ_LOCK(cs); code; }()

/** An implementation of a semaphore.
 *
 * See https://en.wikipedia.org/wiki/Semaphore_(programming)
 */
class CSemaphore
{
private:
    std::condition_variable condition;
    std::mutex mutex;
    int value;

public:
    explicit CSemaphore(int init) noexcept : value(init) {}

    // Disallow default construct, copy, move.
    CSemaphore() = delete;
    CSemaphore(const CSemaphore&) = delete;
    CSemaphore(CSemaphore&&) = delete;
    CSemaphore& operator=(const CSemaphore&) = delete;
    CSemaphore& operator=(CSemaphore&&) = delete;

    void wait() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&]() { return value >= 1; });
        value--;
    }

    bool try_wait() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (value < 1) {
            return false;
        }
        value--;
        return true;
    }

    void post() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            value++;
        }
        condition.notify_one();
    }
};

/** RAII-style semaphore lock */
class CSemaphoreGrant
{
private:
    CSemaphore* sem;
    bool fHaveGrant;

public:
    void Acquire() noexcept
    {
        if (fHaveGrant) {
            return;
        }
        sem->wait();
        fHaveGrant = true;
    }

    void Release() noexcept
    {
        if (!fHaveGrant) {
            return;
        }
        sem->post();
        fHaveGrant = false;
    }

    bool TryAcquire() noexcept
    {
        if (!fHaveGrant && sem->try_wait()) {
            fHaveGrant = true;
        }
        return fHaveGrant;
    }

    // Disallow copy.
    CSemaphoreGrant(const CSemaphoreGrant&) = delete;
    CSemaphoreGrant& operator=(const CSemaphoreGrant&) = delete;

    // Allow move.
    CSemaphoreGrant(CSemaphoreGrant&& other) noexcept
    {
        sem = other.sem;
        fHaveGrant = other.fHaveGrant;
        other.fHaveGrant = false;
        other.sem = nullptr;
    }

    CSemaphoreGrant& operator=(CSemaphoreGrant&& other) noexcept
    {
        Release();
        sem = other.sem;
        fHaveGrant = other.fHaveGrant;
        other.fHaveGrant = false;
        other.sem = nullptr;
        return *this;
    }

    CSemaphoreGrant() noexcept : sem(nullptr), fHaveGrant(false) {}

    explicit CSemaphoreGrant(CSemaphore& sema, bool fTry = false) noexcept : sem(&sema), fHaveGrant(false)
    {
        if (fTry) {
            TryAcquire();
        } else {
            Acquire();
        }
    }

    ~CSemaphoreGrant()
    {
        Release();
    }

    explicit operator bool() const noexcept
    {
        return fHaveGrant;
    }
};

#endif // BITCOIN_SYNC_H
