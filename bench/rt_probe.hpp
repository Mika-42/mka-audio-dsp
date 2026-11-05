/**
 * @file rt_probe.hpp
 * @brief Link-local heap, mutex, wait and output probes for benchmark callbacks.
 * @author Mika
 */
#pragma once
namespace probe {
    /// @brief Whether the thread-local benchmark measurement window is open.
    inline thread_local bool active=false;
    inline thread_local std::uint64_t allocations=0 /**< Number of heap acquisitions recorded inside the measured window. */,
        frees=0 /**< Number of heap releases recorded inside the measured window. */,
        mutexes=0 /**< Number of direct mutex acquisition attempts recorded inside the window. */,
        waits=0 /**< Number of direct blocking/wait calls recorded inside the window. */,
        logs=0 /**< Number of direct output calls recorded inside the measured window. */;
    /// @brief Restore sample guards or observation counters before the next fixture invocation.
    inline void reset() { allocations=frees=mutexes=waits=logs=0; }
}
extern "C" {
/// @brief Linker alias for the unwrapped malloc implementation.
void* __real_malloc(std::size_t); /** Linker alias for the unwrapped free implementation. */ void __real_free(void*);
/// @brief Linker alias for the unwrapped calloc implementation.
void* __real_calloc(std::size_t,std::size_t); /** Linker alias for the unwrapped realloc implementation. */ void* __real_realloc(void*,std::size_t);
/// @brief Count measured malloc calls and forward to the unwrapped implementation.
void* __wrap_malloc(std::size_t n) { if(probe::active) ++probe::allocations; return __real_malloc(n); }
/// @brief Count measured free calls and forward to the unwrapped implementation.
void __wrap_free(void* p) { if(probe::active && p) ++probe::frees; __real_free(p); }
/// @brief Count measured calloc calls and forward to the unwrapped implementation.
void* __wrap_calloc(std::size_t a,std::size_t b) { if(probe::active) ++probe::allocations; return __real_calloc(a,b); }
/// @brief Count measured realloc calls and forward to the unwrapped implementation.
void* __wrap_realloc(void* p,std::size_t n) { if(probe::active) ++probe::allocations; return __real_realloc(p,n); }
/// @brief Linker alias for the unwrapped pthread_mutex_lock implementation.
int __real_pthread_mutex_lock(pthread_mutex_t*); /** Linker alias for the unwrapped pthread_mutex_trylock implementation. */ int __real_pthread_mutex_trylock(pthread_mutex_t*);
/// @brief Linker alias for the unwrapped pthread_mutex_timedlock implementation.
int __real_pthread_mutex_timedlock(pthread_mutex_t*,const timespec*);
/// @brief Count measured pthread_mutex_lock calls and forward to the unwrapped implementation.
int __wrap_pthread_mutex_lock(pthread_mutex_t* p) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_lock(p); }
/// @brief Count measured pthread_mutex_trylock calls and forward to the unwrapped implementation.
int __wrap_pthread_mutex_trylock(pthread_mutex_t* p) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_trylock(p); }
/// @brief Count measured pthread_mutex_timedlock calls and forward to the unwrapped implementation.
int __wrap_pthread_mutex_timedlock(pthread_mutex_t* p,const timespec* t) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_timedlock(p,t); }
/// @brief Linker alias for the unwrapped pthread_cond_wait implementation.
int __real_pthread_cond_wait(pthread_cond_t*,pthread_mutex_t*);
/// @brief Count measured pthread_cond_wait calls and forward to the unwrapped implementation.
int __wrap_pthread_cond_wait(pthread_cond_t* c,pthread_mutex_t* m) { if(probe::active) ++probe::waits; return __real_pthread_cond_wait(c,m); }
/// @brief Linker alias for the unwrapped nanosleep implementation.
int __real_nanosleep(const timespec*,timespec*);
/// @brief Count measured nanosleep calls and forward to the unwrapped implementation.
int __wrap_nanosleep(const timespec* a,timespec* b) { if(probe::active) ++probe::waits; return __real_nanosleep(a,b); }
/// @brief Linker alias for the unwrapped write implementation.
ssize_t __real_write(int,const void*,std::size_t);
/// @brief Count measured write calls and forward to the unwrapped implementation.
ssize_t __wrap_write(int fd,const void* p,std::size_t n) { if(probe::active) ++probe::logs; return __real_write(fd,p,n); }
/// @brief Linker alias for the unwrapped fwrite implementation.
std::size_t __real_fwrite(const void*,std::size_t,std::size_t,FILE*);
/// @brief Count measured fwrite calls and forward to the unwrapped implementation.
std::size_t __wrap_fwrite(const void* p,std::size_t a,std::size_t b,FILE* f) { if(probe::active) ++probe::logs; return __real_fwrite(p,a,b,f); }
}
/// @brief Allocate storage through the instrumented benchmark allocator; allocation failure throws on the calling
/// thread.
void* operator new(std::size_t n) { if(probe::active) ++probe::allocations; if(auto* p=__real_malloc(n?n:1)) return p; throw std::bad_alloc{}; }
/// @brief Allocate storage through the instrumented benchmark allocator; allocation failure throws on the calling
/// thread.
void* operator new[](std::size_t n) { return ::operator new(n); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete(void* p) noexcept { if(probe::active&&p) ++probe::frees; __real_free(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete[](void* p) noexcept { ::operator delete(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
/// @brief Allocate aligned storage through the instrumented benchmark allocator; allocation failure throws on the
/// calling thread.
void* operator new(std::size_t n,std::align_val_t align) {
    if(probe::active) ++probe::allocations;
    const auto a=static_cast<std::size_t>(align); void* p=nullptr;
    if(posix_memalign(&p,a,n?n:1)==0) return p; throw std::bad_alloc{};
}
/// @brief Allocate aligned storage through the instrumented benchmark allocator; allocation failure throws on the
/// calling thread.
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete(void* p,std::align_val_t) noexcept { ::operator delete(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete[](void* p,std::align_val_t) noexcept { ::operator delete(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
/// @brief Release storage through the instrumented benchmark allocator; never propagate exceptions.
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
