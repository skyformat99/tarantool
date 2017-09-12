/*
** 2007 August 28
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains the C functions that implement mutexes for pthreads
*/
#include "sqliteInt.h"

/*
** The code in this file is only used if we are compiling threadsafe
** under unix with pthreads.
**
** Note that this implementation requires a version of pthreads that
** supports recursive mutexes.
*/
#ifdef SQLITE_MUTEX_FIBER

struct sql_fiber_mutex{

};

#include <fiber.h>
#include <fiber_cond.h>

/*
** The sqlite3_mutex.id, sqlite3_mutex.nRef, and sqlite3_mutex.owner fields
** are necessary under two condidtions:  (1) Debug builds and (2) using
** home-grown mutexes.  Encapsulate these conditions into a single #define.
*/
#if defined(SQLITE_DEBUG) || defined(SQLITE_HOMEGROWN_RECURSIVE_MUTEX)
# define SQLITE_MUTEX_NREF 1
#else
# define SQLITE_MUTEX_NREF 0
#endif

/*
** Each recursive mutex is an instance of the following structure.
*/
struct sqlite3_mutex {
  struct fiber_cond c;     /* Mutex controlling the lock */
    unsigned waiters_num;
    int nRef;         /* Number of entrances */
    int owner_fid;    /* 0 if not set, required for recursive mutex */
    int recursive;                    /* Mutex type */
#if SQLITE_MUTEX_NREF
  int trace;                 /* True to trace changes */
#endif
};
#if SQLITE_MUTEX_NREF
#define SQLITE3_MUTEX_INITIALIZER {0,0,0,0,0,0}
#else
#define SQLITE3_MUTEX_INITIALIZER {0,0,0,0,0}
#endif

/*
** The sqlite3_mutex_held() and sqlite3_mutex_notheld() routine are
** intended for use only inside assert() statements.  On some platforms,
** there might be race conditions that can cause these routines to
** deliver incorrect results.  In particular, if pthread_equal() is
** not an atomic operation, then these routines might delivery
** incorrect results.  On most platforms, pthread_equal() is a 
** comparison of two integers and is therefore atomic.  But we are
** told that HPUX is not such a platform.  If so, then these routines
** will not always work correctly on HPUX.
**
** On those platforms where pthread_equal() is not atomic, SQLite
** should be compiled without -DSQLITE_DEBUG and with -DNDEBUG to
** make sure no assert() statements are evaluated and hence these
** routines are never called.
*/
#if !defined(NDEBUG) || defined(SQLITE_DEBUG)
static int fiberMutexHeld(sqlite3_mutex *p){
  return p->nRef!=0 && p->owner_fid==fiber_self()->fid;
}
static int fiberMutexNotheld(sqlite3_mutex *p){
  return p->nRef==0 || p->owner_fid==fiber_self()->fid;
}
#endif

/*
** Try to provide a memory barrier operation, needed for initialization
** and also for the implementation of xShmBarrier in the VFS in cases
** where SQLite is compiled without mutexes.
*/
void sqlite3MemoryBarrier(void){
#if defined(SQLITE_MEMORY_BARRIER)
  SQLITE_MEMORY_BARRIER;
#elif defined(__GNUC__) && GCC_VERSION>=4001000
  __sync_synchronize();
#endif
}

/*
** The sqlite3_mutex_alloc() routine allocates a new
** mutex and returns a pointer to it.  If it returns NULL
** that means that a mutex could not be allocated.  SQLite
** will unwind its stack and return an error.  The argument
** to sqlite3_mutex_alloc() is one of these integer constants:
**
** <ul>
** <li>  SQLITE_MUTEX_FAST
** <li>  SQLITE_MUTEX_RECURSIVE
** <li>  SQLITE_MUTEX_STATIC_MASTER
** <li>  SQLITE_MUTEX_STATIC_MEM
** <li>  SQLITE_MUTEX_STATIC_OPEN
** <li>  SQLITE_MUTEX_STATIC_PRNG
** <li>  SQLITE_MUTEX_STATIC_LRU
** <li>  SQLITE_MUTEX_STATIC_PMEM
** <li>  SQLITE_MUTEX_STATIC_APP1
** <li>  SQLITE_MUTEX_STATIC_APP2
** <li>  SQLITE_MUTEX_STATIC_APP3
** <li>  SQLITE_MUTEX_STATIC_VFS1
** <li>  SQLITE_MUTEX_STATIC_VFS2
** <li>  SQLITE_MUTEX_STATIC_VFS3
** </ul>
**
** The first two constants cause sqlite3_mutex_alloc() to create
** a new mutex.  The new mutex is recursive when SQLITE_MUTEX_RECURSIVE
** is used but not necessarily so when SQLITE_MUTEX_FAST is used.
** The mutex implementation does not need to make a distinction
** between SQLITE_MUTEX_RECURSIVE and SQLITE_MUTEX_FAST if it does
** not want to.  But SQLite will only request a recursive mutex in
** cases where it really needs one.  If a faster non-recursive mutex
** implementation is available on the host platform, the mutex subsystem
** might return such a mutex in response to SQLITE_MUTEX_FAST.
**
** The other allowed parameters to sqlite3_mutex_alloc() each return
** a pointer to a static preexisting mutex.  Six static mutexes are
** used by the current version of SQLite.  Future versions of SQLite
** may add additional static mutexes.  Static mutexes are for internal
** use by SQLite only.  Applications that use SQLite mutexes should
** use only the dynamic mutexes returned by SQLITE_MUTEX_FAST or
** SQLITE_MUTEX_RECURSIVE.
**
** Note that if one of the dynamic mutex parameters (SQLITE_MUTEX_FAST
** or SQLITE_MUTEX_RECURSIVE) is used then sqlite3_mutex_alloc()
** returns a different mutex on every call.  But for the static 
** mutex types, the same mutex is returned on every call that has
** the same type number.
*/
static sqlite3_mutex staticMutexes[] = {
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER,
  SQLITE3_MUTEX_INITIALIZER
};

/*
** Initialize and deinitialize the mutex subsystem.
*/
static int fiberMutexInit(void){
  int i;
  for (i = 0; i < sizeof(staticMutexes); i++){
    /* avoid dynamic mutexes */
    if (i == SQLITE_MUTEX_FAST || i == SQLITE_MUTEX_RECURSIVE){
      continue;
    }
    fiber_cond_create(&staticMutexes[i].c);
  }
  return SQLITE_OK;
}

static int fiberMutexEnd(void){
  int i;
  for (i = 0; i < sizeof(staticMutexes); i++){
    /* avoid dynamic mutexes */
    if (i == SQLITE_MUTEX_FAST || i == SQLITE_MUTEX_RECURSIVE){
      continue;
    }
    fiber_cond_destroy(&staticMutexes[i].c);
  }
  return SQLITE_OK;
}


static sqlite3_mutex *fiberMutexAlloc(int iType){
  sqlite3_mutex *p;
  switch( iType ){
    case SQLITE_MUTEX_RECURSIVE: {
      p = sqlite3MallocZero( sizeof(*p) );
      if( p ){
        fiber_cond_create(&p->c);
        p->recursive = 1;
      }
      break;
    }
    case SQLITE_MUTEX_FAST: {
      p = sqlite3MallocZero( sizeof(*p) );
      if( p ){
        fiber_cond_create(&p->c);
      }
      break;
    }
    default: {
#ifdef SQLITE_ENABLE_API_ARMOR
      if( iType-2<0 || iType-2>=ArraySize(staticMutexes) ){
        (void)SQLITE_MISUSE_BKPT;
        return 0;
      }
#endif
      p = &staticMutexes[iType-2];
      break;
    }
  }
  return p;
}


/*
** This routine deallocates a previously
** allocated mutex.  SQLite is careful to deallocate every
** mutex that it allocates.
*/
static void fiberMutexFree(sqlite3_mutex *p){
  assert( p->nRef==0 );
#if SQLITE_ENABLE_API_ARMOR
  if( p->id==SQLITE_MUTEX_FAST || p->id==SQLITE_MUTEX_RECURSIVE )
#endif
  {
    fiber_cond_destroy(&p->c);
    sqlite3_free(p);
  }
#ifdef SQLITE_ENABLE_API_ARMOR
  else{
    (void)SQLITE_MISUSE_BKPT;
  }
#endif
}

/*
** The sqlite3_mutex_enter() and sqlite3_mutex_try() routines attempt
** to enter a mutex.  If another thread is already within the mutex,
** sqlite3_mutex_enter() will block and sqlite3_mutex_try() will return
** SQLITE_BUSY.  The sqlite3_mutex_try() interface returns SQLITE_OK
** upon successful entry.  Mutexes created using SQLITE_MUTEX_RECURSIVE can
** be entered multiple times by the same thread.  In such cases the,
** mutex must be exited an equal number of times before another thread
** can enter.  If the same thread tries to enter any other kind of mutex
** more than once, the behavior is undefined.
*/
static void fiberMutexEnter(sqlite3_mutex *p){
  struct fiber *self = fiber_self();
  assert(p->recursive || fiberMutexNotheld(p));
  /* recursive and non recursive mutex */
  if (p->owner_fid == self->fid){ /* recursive enter */
    p->nRef++;
    goto fiberNutexEnter_exit;
  }
  if (fiberMutexHeld(p)) { /* should wait */
    p->waiters_num++;
    fiber_cond_wait(&p->c);
    p->waiters_num--;
  }
  /* first fiber in mutex */
  assert(p->nRef==0);
  p->owner_fid = self->fid;
  p->nRef++;
fiberNutexEnter_exit:
#ifdef SQLITE_DEBUG
  if( p->trace ){
    printf("enter mutex %p (%d) with nRef=%d\n", p, p->trace, p->nRef);
  }
#endif
}
static int fiberMutexTry(sqlite3_mutex *p){
  int rc = SQLITE_OK;
  assert( p->recursive || fiberMutexNotheld(p) );
  if (fiberMutexHeld(p)){
    rc = SQLITE_BUSY;
  }else{
    fiberMutexEnter(p);
  }
#ifdef SQLITE_DEBUG
  if( rc==SQLITE_OK && p->trace ){
    printf("enter mutex %p (%d) with nRef=%d\n", p, p->trace, p->nRef);
  }
#endif
  return rc;
}

/*
** The sqlite3_mutex_leave() routine exits a mutex that was
** previously entered by the same thread.  The behavior
** is undefined if the mutex is not currently entered or
** is not currently allocated.  SQLite will never do either.
*/
static void fiberMutexLeave(sqlite3_mutex *p){
  assert( fiberMutexHeld(p) );
  p->nRef--;
  if (p->recursive && p->nRef){
      goto fiberMutexLeave_exit;
  }
  assert(p->nRef==0);
  p->owner_fid = 0;
  fiber_cond_signal(&p->c);
fiberMutexLeave_exit:
#ifdef SQLITE_DEBUG
  if( p->trace ){
    printf("leave mutex %p (%d) with nRef=%d\n", p, p->trace, p->nRef);
  }
#endif
}

sqlite3_mutex_methods const *sqlite3DefaultMutex(void){
  static const sqlite3_mutex_methods sMutex = {
    fiberMutexInit,
    fiberMutexEnd,
    fiberMutexAlloc,
    fiberMutexFree,
    fiberMutexEnter,
    fiberMutexTry,
    fiberMutexLeave,
#ifdef SQLITE_DEBUG
    fiberMutexHeld,
    fiberMutexNotheld
#else
    0,
    0
#endif
  };

  return &sMutex;
}

#endif /* SQLITE_MUTEX_FIBER */
