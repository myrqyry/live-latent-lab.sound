// Positive control built as a separate shared library, so its calls cross
// the same dynamic-linker boundary as calls from the Sound module.
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <assert.h>
void exercise_rt_probe(void){
    void* p=malloc(32);assert(p);p=realloc(p,64);assert(p);free(p);
    p=calloc(2,32);assert(p);free(p);
    p=aligned_alloc(64,64);assert(p);free(p);
    assert(posix_memalign(&p,64,64)==0);free(p);
    pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
    pthread_rwlock_t rw=PTHREAD_RWLOCK_INITIALIZER;
    pthread_cond_t c=PTHREAD_COND_INITIALIZER;
    assert(pthread_mutex_lock(&m)==0);
    struct timespec expired={0,0};
    assert(pthread_cond_timedwait(&c,&m,&expired)!=0);
    pthread_mutex_unlock(&m);
    assert(pthread_rwlock_rdlock(&rw)==0);pthread_rwlock_unlock(&rw);
    assert(pthread_rwlock_wrlock(&rw)==0);pthread_rwlock_unlock(&rw);
    pthread_cond_destroy(&c);pthread_rwlock_destroy(&rw);pthread_mutex_destroy(&m);
}
