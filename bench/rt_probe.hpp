#pragma once
namespace probe {
    inline thread_local bool active=false;
    inline thread_local std::uint64_t allocations=0,frees=0,mutexes=0,waits=0,logs=0;
    inline void reset() { allocations=frees=mutexes=waits=logs=0; }
}
extern "C" {
void* __real_malloc(std::size_t); void __real_free(void*);
void* __real_calloc(std::size_t,std::size_t); void* __real_realloc(void*,std::size_t);
void* __wrap_malloc(std::size_t n) { if(probe::active) ++probe::allocations; return __real_malloc(n); }
void __wrap_free(void* p) { if(probe::active && p) ++probe::frees; __real_free(p); }
void* __wrap_calloc(std::size_t a,std::size_t b) { if(probe::active) ++probe::allocations; return __real_calloc(a,b); }
void* __wrap_realloc(void* p,std::size_t n) { if(probe::active) ++probe::allocations; return __real_realloc(p,n); }
int __real_pthread_mutex_lock(pthread_mutex_t*); int __real_pthread_mutex_trylock(pthread_mutex_t*);
int __real_pthread_mutex_timedlock(pthread_mutex_t*,const timespec*);
int __wrap_pthread_mutex_lock(pthread_mutex_t* p) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_lock(p); }
int __wrap_pthread_mutex_trylock(pthread_mutex_t* p) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_trylock(p); }
int __wrap_pthread_mutex_timedlock(pthread_mutex_t* p,const timespec* t) { if(probe::active) ++probe::mutexes; return __real_pthread_mutex_timedlock(p,t); }
int __real_pthread_cond_wait(pthread_cond_t*,pthread_mutex_t*);
int __wrap_pthread_cond_wait(pthread_cond_t* c,pthread_mutex_t* m) { if(probe::active) ++probe::waits; return __real_pthread_cond_wait(c,m); }
int __real_nanosleep(const timespec*,timespec*);
int __wrap_nanosleep(const timespec* a,timespec* b) { if(probe::active) ++probe::waits; return __real_nanosleep(a,b); }
ssize_t __real_write(int,const void*,std::size_t);
ssize_t __wrap_write(int fd,const void* p,std::size_t n) { if(probe::active) ++probe::logs; return __real_write(fd,p,n); }
std::size_t __real_fwrite(const void*,std::size_t,std::size_t,FILE*);
std::size_t __wrap_fwrite(const void* p,std::size_t a,std::size_t b,FILE* f) { if(probe::active) ++probe::logs; return __real_fwrite(p,a,b,f); }
}
void* operator new(std::size_t n) { if(probe::active) ++probe::allocations; if(auto* p=__real_malloc(n?n:1)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if(probe::active&&p) ++probe::frees; __real_free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n,std::align_val_t align) {
    if(probe::active) ++probe::allocations;
    const auto a=static_cast<std::size_t>(align); void* p=nullptr;
    if(posix_memalign(&p,a,n?n:1)==0) return p; throw std::bad_alloc{};
}
void* operator new[](std::size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept { ::operator delete(p); }
