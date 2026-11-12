// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A pthread mutex held for the lifetime of a scope.
#ifndef AA_ENTRYNAV_CORE_MUTEX_LOCK_HPP
#define AA_ENTRYNAV_CORE_MUTEX_LOCK_HPP

#include <pthread.h>

namespace aa {

class MutexLock {
public:
    explicit MutexLock(pthread_mutex_t& mutex) : mutex_(mutex) { pthread_mutex_lock(&mutex_); }
    ~MutexLock() { pthread_mutex_unlock(&mutex_); }

private:
    MutexLock(const MutexLock&);
    MutexLock& operator=(const MutexLock&);
    pthread_mutex_t& mutex_;
};

}  // namespace aa

#endif  // AA_ENTRYNAV_CORE_MUTEX_LOCK_HPP
