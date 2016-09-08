// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2008-2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#include "BackTrace.h"
#include "Clock.h"
#include "common/dout.h"
#include "common/environment.h"
#include "common/valgrind.h"
#include "include/types.h"
#include "lockdep.h"

#include "include/unordered_map.h"

#if defined(__FreeBSD__) && defined(__LP64__)	// On FreeBSD pthread_t is a pointer.
namespace std {
  template<>
    struct hash<pthread_t>
    {
      size_t
      operator()(pthread_t __x) const
      { return (uintptr_t)__x; }
    };
} // namespace std
#endif

/******* Constants **********/
#define lockdep_dout(v) lsubdout(g_lockdep_ceph_ctx, lockdep, v)
#define MAX_LOCKS  4096   // increase me as needed
#define BACKTRACE_SKIP 2

/******* Globals **********/
int g_lockdep = 0;
struct lockdep_stopper_t {
  // disable lockdep when this module destructs.
  ~lockdep_stopper_t() {
    g_lockdep = 0;
  }
};
static pthread_mutex_t lockdep_mutex = PTHREAD_MUTEX_INITIALIZER;
static CephContext *g_lockdep_ceph_ctx = NULL;
static lockdep_stopper_t lockdep_stopper;
static ceph::unordered_map<std::string, int> lock_ids;
static map<int, std::string> lock_names;
static map<int, int> lock_refs;
static char free_ids[MAX_LOCKS/8]; // bit set = free
static ceph::unordered_map<pthread_t, map<int,BackTrace*> > held;
static char follows[MAX_LOCKS][MAX_LOCKS/8]; // follows[a][b] means b taken after a
static BackTrace *follows_bt[MAX_LOCKS][MAX_LOCKS];
unsigned current_maxid;
int last_freed_id;

static bool lockdep_force_backtrace()
{
  return (g_lockdep_ceph_ctx != NULL &&
          g_lockdep_ceph_ctx->_conf->lockdep_force_backtrace);
}

/******* Functions **********/
void lockdep_register_ceph_context(CephContext *cct)
{
  static_assert((MAX_LOCKS > 0) && (MAX_LOCKS % 8 == 0),                   
    "lockdep's MAX_LOCKS needs to be divisible by 8 to operate correctly.");
  pthread_mutex_lock(&lockdep_mutex);
  if (g_lockdep_ceph_ctx == NULL) {
    ANNOTATE_BENIGN_RACE_SIZED(&g_lockdep_ceph_ctx, sizeof(g_lockdep_ceph_ctx),
                               "lockdep cct");
    ANNOTATE_BENIGN_RACE_SIZED(&g_lockdep, sizeof(g_lockdep),
                               "lockdep enabled");
    g_lockdep = true;
    g_lockdep_ceph_ctx = cct;
    lockdep_dout(1) << "lockdep start" << dendl;
    current_maxid = 0;
	last_freed_id = -1;

    memset((void*) &free_ids[0], 255, sizeof(free_ids));
  }
  pthread_mutex_unlock(&lockdep_mutex);
}

void lockdep_unregister_ceph_context(CephContext *cct)
{
  pthread_mutex_lock(&lockdep_mutex);
  if (cct == g_lockdep_ceph_ctx) {
    lockdep_dout(1) << "lockdep stop" << dendl;
    // this cct is going away; shut it down!
    g_lockdep = false;
    g_lockdep_ceph_ctx = NULL;

    // blow away all of our state, too, in case it starts up again.
    for (unsigned i = 0; i < current_maxid; ++i) {
      for (unsigned j = 0; j < current_maxid; ++j) {
        delete follows_bt[i][j];
      }
    }

    held.clear();
    lock_names.clear();
    lock_ids.clear();
    lock_refs.clear();
    memset((void*)&free_ids[0], 0, sizeof(free_ids));
    memset((void*)&follows[0][0], 0, current_maxid * MAX_LOCKS/8);
    memset((void*)&follows_bt[0][0], 0, sizeof(BackTrace*) * current_maxid * MAX_LOCKS);
    current_maxid = 0;
    last_freed_id = -1;
  }
  pthread_mutex_unlock(&lockdep_mutex);
}

int lockdep_dump_locks()
{
  pthread_mutex_lock(&lockdep_mutex);

  for (ceph::unordered_map<pthread_t, map<int,BackTrace*> >::iterator p = held.begin();
       p != held.end();
       ++p) {
    lockdep_dout(0) << "--- thread " << p->first << " ---" << dendl;
    for (map<int,BackTrace*>::iterator q = p->second.begin();
	 q != p->second.end();
	 ++q) {
      lockdep_dout(0) << "  * " << lock_names[q->first] << "\n";
      if (q->second)
	q->second->print(*_dout);
      *_dout << dendl;
    }
  }

  pthread_mutex_unlock(&lockdep_mutex);
  return 0;
}

int lockdep_get_free_id(void)
{
  // if there's id known to be freed lately, reuse it
  if ((last_freed_id >= 0) && 
     (free_ids[last_freed_id/8] & (1 << (last_freed_id % 8)))) {
    int tmp = last_freed_id;
    last_freed_id = -1;
    free_ids[tmp/8] &= 255 - (1 << (tmp % 8));
    lockdep_dout(1) << "reusing last freed id " << tmp << dendl;
    return tmp;
  }
  
  // walk through entire array and locate nonzero char, then find
  // actual bit.
  for (int i = 0; i < MAX_LOCKS / 8; ++i) {
    if (free_ids[i] != 0) {
      for (int j = 0; j < 8; ++j) {
        if (free_ids[i] & (1 << j)) {
          free_ids[i] &= 255 - (1 << j);
          lockdep_dout(1) << "using id " << i * 8 + j << dendl;
          return i * 8 + j;
        }
      }
    }
  }
  
  // not found
  lockdep_dout(0) << "failing miserably..." << dendl;
  return -1;
}

int lockdep_register(const char *name)
{
  int id;

  pthread_mutex_lock(&lockdep_mutex);
  ceph::unordered_map<std::string, int>::iterator p = lock_ids.find(name);
  if (p == lock_ids.end()) {
    id = lockdep_get_free_id();
    if (id < 0) {
      lockdep_dout(0) << "ERROR OUT OF IDS .. have 0"
		      << " max " << MAX_LOCKS << dendl;
      for (auto& p : lock_names) {
	lockdep_dout(0) << "  lock " << p.first << " " << p.second << dendl;
      }
      assert(false);
    }
    if (current_maxid <= (unsigned)id) {
        current_maxid = (unsigned)id + 1;
    }
    lock_ids[name] = id;
    lock_names[id] = name;
    lockdep_dout(10) << "registered '" << name << "' as " << id << dendl;
  } else {
    id = p->second;
    lockdep_dout(20) << "had '" << name << "' as " << id << dendl;
  }

  ++lock_refs[id];
  pthread_mutex_unlock(&lockdep_mutex);

  return id;
}

void lockdep_unregister(int id)
{
  if (id < 0) {
    return;
  }

  pthread_mutex_lock(&lockdep_mutex);

  map<int, std::string>::iterator p = lock_names.find(id);
  assert(p != lock_names.end());

  int &refs = lock_refs[id];
  if (--refs == 0) {
    // reset dependency ordering
    memset((void*)&follows[id][0], 0, MAX_LOCKS/8);
    for (unsigned i=0; i<current_maxid; ++i) {
      delete follows_bt[id][i];
      follows_bt[id][i] = NULL;

      delete follows_bt[i][id];
      follows_bt[i][id] = NULL;
      follows[i][id / 8] &= 255 - (1 << (id % 8));
    }

    lockdep_dout(10) << "unregistered '" << p->second << "' from " << id
                     << dendl;
    lock_ids.erase(p->second);
    lock_names.erase(id);
    lock_refs.erase(id);
    free_ids[id/8] |= (1 << (id % 8));
	last_freed_id = id;
  } else {
    lockdep_dout(20) << "have " << refs << " of '" << p->second << "' "
                     << "from " << id << dendl;
  }
  pthread_mutex_unlock(&lockdep_mutex);
}


// does b follow a?
static bool does_follow(int a, int b)
{
  if (follows[a][b/8] & (1 << (b % 8))) {
    lockdep_dout(0) << "\n";
    *_dout << "------------------------------------" << "\n";
    *_dout << "existing dependency " << lock_names[a] << " (" << a << ") -> "
           << lock_names[b] << " (" << b << ") at:\n";
    if (follows_bt[a][b]) {
      follows_bt[a][b]->print(*_dout);
    }
    *_dout << dendl;
    return true;
  }

  for (unsigned i=0; i<current_maxid; i++) {
    if ((follows[a][i/8] & (1 << (i % 8))) &&
	does_follow(i, b)) {
      lockdep_dout(0) << "existing intermediate dependency " << lock_names[a]
          << " (" << a << ") -> " << lock_names[i] << " (" << i << ") at:\n";
      if (follows_bt[a][i]) {
        follows_bt[a][i]->print(*_dout);
      }
      *_dout << dendl;
      return true;
    }
  }

  return false;
}

int lockdep_will_lock(const char *name, int id, bool force_backtrace)
{
  pthread_t p = pthread_self();
  if (id < 0) id = lockdep_register(name);

  pthread_mutex_lock(&lockdep_mutex);
  lockdep_dout(20) << "_will_lock " << name << " (" << id << ")" << dendl;

  // check dependency graph
  map<int, BackTrace *> &m = held[p];
  for (map<int, BackTrace *>::iterator p = m.begin();
       p != m.end();
       ++p) {
    if (p->first == id) {
      lockdep_dout(0) << "\n";
      *_dout << "recursive lock of " << name << " (" << id << ")\n";
      BackTrace *bt = new BackTrace(BACKTRACE_SKIP);
      bt->print(*_dout);
      if (p->second) {
	*_dout << "\npreviously locked at\n";
	p->second->print(*_dout);
      }
      delete bt;
      *_dout << dendl;
      assert(0);
    }
    else if (!(follows[p->first][id/8] & (1 << (id % 8)))) {
      // new dependency

      // did we just create a cycle?
      if (does_follow(id, p->first)) {
        BackTrace *bt = new BackTrace(BACKTRACE_SKIP);
	lockdep_dout(0) << "new dependency " << lock_names[p->first]
		<< " (" << p->first << ") -> " << name << " (" << id << ")"
		<< " creates a cycle at\n";
	bt->print(*_dout);
	*_dout << dendl;

	lockdep_dout(0) << "btw, i am holding these locks:" << dendl;
	for (map<int, BackTrace *>::iterator q = m.begin();
	     q != m.end();
	     ++q) {
	  lockdep_dout(0) << "  " << lock_names[q->first] << " (" << q->first << ")" << dendl;
	  if (q->second) {
	    lockdep_dout(0) << " ";
	    q->second->print(*_dout);
	    *_dout << dendl;
	  }
	}

	lockdep_dout(0) << "\n" << dendl;

	// don't add this dependency, or we'll get aMutex. cycle in the graph, and
	// does_follow() won't terminate.

	assert(0);  // actually, we should just die here.
      } else {
        BackTrace *bt = NULL;
        if (force_backtrace || lockdep_force_backtrace()) {
          bt = new BackTrace(BACKTRACE_SKIP);
        }
        follows[p->first][id/8] |= 1 << (id % 8);
        follows_bt[p->first][id] = bt;
	lockdep_dout(10) << lock_names[p->first] << " -> " << name << " at" << dendl;
	//bt->print(*_dout);
      }
    }
  }

  pthread_mutex_unlock(&lockdep_mutex);
  return id;
}

int lockdep_locked(const char *name, int id, bool force_backtrace)
{
  pthread_t p = pthread_self();

  if (id < 0) id = lockdep_register(name);

  pthread_mutex_lock(&lockdep_mutex);
  lockdep_dout(20) << "_locked " << name << dendl;
  if (force_backtrace || lockdep_force_backtrace())
    held[p][id] = new BackTrace(BACKTRACE_SKIP);
  else
    held[p][id] = 0;
  pthread_mutex_unlock(&lockdep_mutex);
  return id;
}

int lockdep_will_unlock(const char *name, int id)
{
  pthread_t p = pthread_self();

  if (id < 0) {
    //id = lockdep_register(name);
    assert(id == -1);
    return id;
  }

  pthread_mutex_lock(&lockdep_mutex);
  lockdep_dout(20) << "_will_unlock " << name << dendl;

  // don't assert.. lockdep may be enabled at any point in time
  //assert(held.count(p));
  //assert(held[p].count(id));

  delete held[p][id];
  held[p].erase(id);
  pthread_mutex_unlock(&lockdep_mutex);
  return id;
}


