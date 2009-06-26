// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <iostream>
#include <string>
using namespace std;

#include "config.h"

#include "mon/MonMap.h"
#include "mds/MDS.h"

#include "msg/SimpleMessenger.h"

#include "common/Timer.h"
#include "common/common_init.h"

#include "mon/MonClient.h"

void usage()
{
  cerr << "usage: cmds -i name [flags] [--mds rank] [--shadow rank]\n";
  cerr << "  -m monitorip:port\n";
  cerr << "        connect to monitor at given address\n";
  cerr << "  --debug_mds n\n";
  cerr << "        debug MDS level (e.g. 10)\n";
  generic_server_usage();
}

int main(int argc, const char **argv) 
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);
  common_init(args, "mds", true);

  // mds specific args
  for (unsigned i=0; i<args.size(); i++) {
    cerr << "unrecognized arg " << args[i] << std::endl;
    usage();
  }
  if (!g_conf.id) {
    cerr << "must specify '-i name' with the cmds instance name" << std::endl;
    usage();
  }

  if (g_conf.clock_tare) g_clock.tare();

  // get monmap
  MonClient mc;
  if (mc.build_initial_monmap() < 0)
    return -1;

  SimpleMessenger rank;
  rank.bind();
  cout << "starting mds." << g_conf.id
       << " at " << rank.get_rank_addr() 
       << std::endl;

  Messenger *m = rank.register_entity(entity_name_t::MDS(-1));
  assert_warn(m);
  if (!m)
    return 1;

  rank.set_policy(entity_name_t::TYPE_MON, SimpleMessenger::Policy::lossy_fail_after(1.0));
  rank.set_policy(entity_name_t::TYPE_MDS, SimpleMessenger::Policy::lossless());
  rank.set_policy(entity_name_t::TYPE_OSD, SimpleMessenger::Policy::lossless());
  rank.set_policy(entity_name_t::TYPE_CLIENT, SimpleMessenger::Policy::lossless());  // mds does its own timeout/markdown

  rank.start();
  
  // start mds
  MDS *mds = new MDS(g_conf.id, m, &mc.monmap);
  mds->init();
  
  rank.wait();

  // yuck: grab the mds lock, so we can be sure that whoever in *mds 
  // called shutdown finishes what they were doing.
  mds->mds_lock.Lock();
  mds->mds_lock.Unlock();

  // only delete if it was a clean shutdown (to aid memory leak
  // detection, etc.).  don't bother if it was a suicide.
  if (mds->is_stopped())
    delete mds;

  // cd on exit, so that gmon.out (if any) goes into a separate directory for each node.
  char s[20];
  sprintf(s, "gmon/%d", getpid());
  if (mkdir(s, 0755) == 0)
    chdir(s);

  generic_dout(0) << "stopped." << dendl;
  return 0;
}

