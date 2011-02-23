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

#include "include/types.h"

#include "include/rados/librados.hpp"
using namespace librados;

#include "osdc/rados_bencher.h"

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "common/Cond.h"
#include "mds/inode_backtrace.h"
#include <iostream>
#include <fstream>

#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <errno.h>

void usage() 
{
  cerr << "usage: rados [options] [commands]" << std::endl;
  /*  cerr << "If no commands are specified, enter interactive mode.\n";
  cerr << "Commands:" << std::endl;
  cerr << "   stop              -- cleanly shut down file system" << std::endl
       << "   (osd|pg|mds) stat -- get monitor subsystem status" << std::endl
       << "   ..." << std::endl;
  */
  cerr << "Commands:\n";
  cerr << "   lspools     -- list pools\n";
  cerr << "   df          -- show per-pool and total usage\n\n";

  cerr << "Pool commands:\n";
  cerr << "   get objname [outfile] -- fetch object\n";
  cerr << "   put objname [infile] -- write object\n";
  cerr << "   create objname -- create object\n";
  cerr << "   rm objname  -- remove object\n";
  cerr << "   listxattr objname\n";
  cerr << "   getxattr objname attr\n";
  cerr << "   setxattr objname attr val\n";
  cerr << "   rmxattr objname attr\n";
  cerr << "   ls          -- list objects in pool\n\n";
  cerr << "   chown 123   -- change the pool owner to auid 123\n";
  cerr << "   mapext objname\n";

  cerr << "   mkpool foo [123[ 4]]  -- create pool 'foo'\n"
       << "                         [with auid 123[and using crush rule 4]]\n";
  cerr << "   rmpool foo  -- remove pool 'foo'\n";
  cerr << "   mkpool foo  -- create the pool 'foo'\n";
  cerr << "   lssnap      -- list snaps\n";
  cerr << "   mksnap foo  -- create snap 'foo'\n";
  cerr << "   rmsnap foo  -- remove snap 'foo'\n";
  cerr << "   rollback foo bar -- roll back object foo to snap 'bar'\n\n";

  cerr << "   bench <seconds> write|seq|rand [-t concurrent_operations]\n";
  cerr << "              default is 16 concurrent IOs and 4 MB op size\n\n";

  cerr << "Options:\n";
  cerr << "   -p pool\n";
  cerr << "   --pool=pool\n";
  cerr << "        select given pool by name\n";
  cerr << "   -b op_size\n";
  cerr << "        set the size of write ops for put or benchmarking";
  cerr << "   -s name\n";
  cerr << "   --snap name\n";
  cerr << "        select given snap name for (read) IO\n";
  cerr << "   -i infile\n";
  cerr << "   -o outfile\n";
  cerr << "        specify input or output file (for certain commands)\n";
  exit(1);
}


/**********************************************

**********************************************/

int main(int argc, const char **argv) 
{
  DEFINE_CONF_VARS(usage);
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  common_init(args, "rados",
	      STARTUP_FLAG_FORCE_FG_LOGGING | STARTUP_FLAG_INIT_KEYS);

  vector<const char*> nargs;
  bufferlist indata, outdata;

  const char *pool = 0;
 
  int concurrent_ios = 16;
  int op_size = 1 << 22;

  const char *snapname = 0;
  snap_t snapid = CEPH_NOSNAP;

  const char *filter = NULL;

  FOR_EACH_ARG(args) {
    if (CONF_ARG_EQ("pool", 'p')) {
      CONF_SAFE_SET_ARG_VAL(&pool, OPT_STR);
    } else if (CONF_ARG_EQ("snapid", 'S')) {
      CONF_SAFE_SET_ARG_VAL(&snapid, OPT_LONGLONG);
    } else if (CONF_ARG_EQ("snap", 's')) {
      CONF_SAFE_SET_ARG_VAL(&snapname, OPT_STR);
    } else if (CONF_ARG_EQ("help", 'h')) {
      usage();
    } else if (CONF_ARG_EQ("concurrent-ios", 't')) {
      CONF_SAFE_SET_ARG_VAL(&concurrent_ios, OPT_INT);
    } else if (CONF_ARG_EQ("block-size", 'b')) {
      CONF_SAFE_SET_ARG_VAL(&op_size, OPT_INT);
    } else if (CONF_ARG_EQ("filter", '\0')) {
      CONF_SAFE_SET_ARG_VAL(&filter, OPT_STR);
    } else if (args[i][0] == '-' && nargs.empty()) {
      cerr << "unrecognized option " << args[i] << std::endl;
      usage();
    } else
      nargs.push_back(args[i]);
  }

  if (nargs.empty())
    usage();

  // open rados
  Rados rados;
  if (rados.initialize(0, NULL) < 0) {
     cerr << "couldn't initialize rados!" << std::endl;
     exit(1);
  }

  int ret = 0;
  char buf[80];

  // open pool?
  pool_t p;
  if (pool) {
    ret = rados.open_pool(pool, &p);
    if (ret < 0) {
      cerr << "error opening pool " << pool << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto no_pool_out;
    }
  }

  // snapname?
  if (snapname) {
    ret = rados.snap_lookup(p, snapname, &snapid);
    if (ret < 0) {
      cerr << "error looking up snap '" << snapname << "': " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
  }
  if (snapid != CEPH_NOSNAP) {
    string name;
    ret = rados.snap_get_name(p, snapid, &name);
    if (ret < 0) {
      cerr << "snapid " << snapid << " doesn't exist in pool " << pool << std::endl;
      goto out;
    }
    rados.set_snap_read(p, snapid);
    cout << "selected snap " << snapid << " '" << snapname << "'" << std::endl;
  }

  // list pools?
  if (strcmp(nargs[0], "lspools") == 0) {
    list<string> vec;
    rados.pool_list(vec);
    for (list<string>::iterator i = vec.begin(); i != vec.end(); ++i)
      cout << *i << std::endl;
  }
  else if (strcmp(nargs[0], "df") == 0) {
    // pools
    list<string> vec;
    rados.pool_list(vec);
    
    map<string,pool_stat_t> stats;
    rados.pool_get_stats(vec, stats);

    printf("%-15s "
	   "%12s %12s %12s %12s "
	   "%12s %12s %12s %12s %12s\n",
	   "pool name",
	   "KB", "objects", "clones", "degraded",
	   "unfound", "rd", "rd KB", "wr", "wr KB");
    for (map<string,pool_stat_t>::iterator i = stats.begin(); i != stats.end(); ++i) {
      printf("%-15s "
	     "%12lld %12lld %12lld %12lld"
	     "%12lld %12lld %12lld %12lld %12lld\n",
	     i->first.c_str(),
	     (long long)i->second.num_kb,
	     (long long)i->second.num_objects,
	     (long long)i->second.num_object_clones,
	     (long long)i->second.num_objects_degraded,
	     (long long)i->second.num_objects_unfound,
	     (long long)i->second.num_rd, (long long)i->second.num_rd_kb,
	     (long long)i->second.num_wr, (long long)i->second.num_wr_kb);
    }

    // total
    statfs_t tstats;
    rados.fs_get_stats(tstats);
    printf("  total used    %12lld %12lld\n", (long long unsigned)tstats.kb_used,
	   (long long unsigned)tstats.num_objects);
    printf("  total avail   %12lld\n", (long long unsigned)tstats.kb_avail);
    printf("  total space   %12lld\n", (long long unsigned)tstats.kb);
  }

  else if (strcmp(nargs[0], "ls") == 0) {
    if (!pool) {
      cerr << "pool name was not specified" << std::endl;
      goto out;
    }

    bool stdout = (nargs.size() < 2) || (strcmp(nargs[1], "-") == 0);
    ostream *outstream;
    if(stdout)
      outstream = &cout;
    else
      outstream = new ofstream(nargs[1]);

    Rados::ListCtx ctx;
    rados.objects_list_open(p, &ctx);
    bufferlist extra_info;
    bool filter_parent = false;
    if (filter) {
      char *flt_str = strdup(filter);
      char *type = strtok(flt_str, " ");
      if (!type) {
        cerr << "filter type was not specified" << std::endl;
        goto out;
      }
      char *xattr = NULL;
      if (strcmp(type, "parent") != 0)
        xattr = strtok(NULL, " ");
      char *val = strtok(NULL, " ");

      if (!val) {
        cerr << "filter was not specified correctly" << std::endl;
        goto out;
      }

      bufferlist bl;
      ::encode(type, bl);
      if (strcmp(type, "parent") ==  0) {
        inodeno_t int_val = strtoll(val, NULL, 0);
        ::encode(int_val, bl);
        filter_parent = true;
      } else if (strcmp(type, "plain") == 0) {
        ::encode(xattr, bl);
        ::encode(val, bl);
      } else {
        cerr << "unknown filter type" << std::endl;
        goto out;
      }

      rados.list_filter(ctx, bl, &extra_info);
    }
    while (1) {
      list<string> vec;
      ret = rados.objects_list_more(ctx, 1 << 10, vec);
      if (ret < 0) {
	cerr << "got error: " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
	delete outstream;
	goto out;
      }
      if (vec.empty())
	break;

      bufferlist::iterator exiter = extra_info.begin();
      for (list<string>::iterator iter = vec.begin(); iter != vec.end(); ++iter) {
	*outstream << *iter << std::endl;
        if (filter_parent) {
          inode_backpointer_t backp;
          ::decode(backp, exiter);
          cout << " dirino=" << backp.dirino << " dname=" << backp.dname << " v=" << backp.version << std::endl;
        }
      }
    }
    rados.objects_list_close(ctx);
    if (!stdout)
      delete outstream;
  }
  else if (strcmp(nargs[0], "chown") == 0) {
    if (!pool || nargs.size() < 2)
      usage();

    uint64_t new_auid = strtol(nargs[1], 0, 10);
    ret = rados.change_pool_auid(p, new_auid);
    if (ret < 0) {
      cerr << "error changing auid on pool " << pool << ':'
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
    } else cerr << "changed auid on pool " << pool
		<< " to " << new_auid << std::endl;
  }
  else if (strcmp(nargs[0], "mapext") == 0) {
    if (!pool || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    std::map<off_t, size_t> m;
    ret = rados.mapext(p, oid, 0, -1, m);
    if (ret < 0) {
      cerr << "mapext error on " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    std::map<off_t, size_t>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      cout << hex << iter->first << "\t" << iter->second << dec << std::endl;
    }
  }
  else if (strcmp(nargs[0], "get") == 0) {
    if (!pool || nargs.size() < 3)
      usage();
    string oid(nargs[1]);
    ret = rados.read(p, oid, 0, outdata, 0);
    if (ret < 0) {
      cerr << "error reading " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }

    if (strcmp(nargs[2], "-") == 0) {
      fwrite(outdata.c_str(), outdata.length(), 1, stdout);
    } else {
      outdata.write_file(nargs[2]);
      generic_dout(0) << "wrote " << outdata.length() << " byte payload to " << nargs[2] << dendl;
    }
  }
  else if (strcmp(nargs[0], "put") == 0) {
    if (!pool || nargs.size() < 3)
      usage();

    string oid(nargs[1]);
    bool stdio = false;
    if (strcmp(nargs[2], "-") == 0)
      stdio = true;

    if (stdio) {
      char buf[256];
      while(!cin.eof()) {
	cin.getline(buf, 256);
	indata.append(buf);
	indata.append('\n');
      }
    } else {
      int fd = open(nargs[2], O_RDONLY);
      if (fd < 0) {
	cerr << "error reading input file " << nargs[2] << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
	goto out;
      }
      char buf[op_size];
      int count = op_size;
      uint64_t offset = 0;
      while (count == op_size) {
        count = read(fd, buf, op_size);
        if (count == 0)
          continue;
        indata.append(buf, count);
        ret = rados.write(p, oid, offset, indata, count);
        indata.clear();

        if (ret < 0) {
          cerr << "error writing " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
          goto out;
        }
        offset += count;
      }
    }
  }
  else if (strcmp(nargs[0], "setxattr") == 0) {
    if (!pool || nargs.size() < 4)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);
    string attr_val(nargs[3]);

    bufferlist bl;
    bl.append(attr_val.c_str(), attr_val.length());

    ret = rados.setxattr(p, oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error setting xattr " << pool << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
  }
  else if (strcmp(nargs[0], "getxattr") == 0) {
    if (!pool || nargs.size() < 3)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    bufferlist bl;
    ret = rados.getxattr(p, oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error getting xattr " << pool << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    string s(bl.c_str(), bl.length());
    cout << s << std::endl;
  } else if (strcmp(nargs[0], "rmxattr") == 0) {
    if (!pool || nargs.size() < 3)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    ret = rados.rmxattr(p, oid, attr_name.c_str());
    if (ret < 0) {
      cerr << "error removing xattr " << pool << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
  } else if (strcmp(nargs[0], "listxattr") == 0) {
    if (!pool || nargs.size() < 2)
      usage();

    string oid(nargs[1]);
    map<std::string, bufferlist> attrset;
    bufferlist bl;
    ret = rados.getxattrs(p, oid, attrset);
    if (ret < 0) {
      cerr << "error getting xattr set " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }

    for (map<std::string, bufferlist>::iterator iter = attrset.begin();
         iter != attrset.end(); ++iter) {
      cout << iter->first << std::endl;
    }
  }
  else if (strcmp(nargs[0], "rm") == 0) {
    if (!pool || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    ret = rados.remove(p, oid);
    if (ret < 0) {
      cerr << "error removing " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
  }
  else if (strcmp(nargs[0], "create") == 0) {
    if (!pool || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    ret = rados.create(p, oid, true);
    if (ret < 0) {
      cerr << "error creating " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
  }

  else if (strcmp(nargs[0], "tmap") == 0) {
    if (nargs.size() < 3)
      usage();
    if (strcmp(nargs[1], "dump") == 0) {
      string oid(nargs[2]);
      ret = rados.read(p, oid, 0, outdata, 0);
      if (ret < 0) {
	cerr << "error reading " << pool << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
	goto out;
      }
      bufferlist::iterator p = outdata.begin();
      bufferlist header;
      map<string, bufferlist> kv;
      ::decode(header, p);
      ::decode(kv, p);
      cout << "header (" << header.length() << " bytes):\n";
      header.hexdump(cout);
      cout << "\n";
      cout << kv.size() << " keys\n";
      for (map<string,bufferlist>::iterator q = kv.begin(); q != kv.end(); q++) {
	cout << "key '" << q->first << "' (" << q->second.length() << " bytes):\n";
	q->second.hexdump(cout);
	cout << "\n";
      }
    }    
  }

  else if (strcmp(nargs[0], "mkpool") == 0) {
    int auid = 0;
    __u8 crush_rule = 0;
    if (nargs.size() < 2)
      usage();
    if (nargs.size() > 2) {
      auid = strtol(nargs[2], 0, 10);
      cerr << "setting auid:" << auid << std::endl;
      if (nargs.size() > 3) {
	crush_rule = (__u8)strtol(nargs[3], 0, 10);
	cerr << "using crush rule " << (int)crush_rule << std::endl;
      }
    }
    ret = rados.create_pool(nargs[1], auid, crush_rule);
    if (ret < 0) {
      cerr << "error creating pool " << nargs[1] << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    cout << "successfully created pool " << nargs[1] << std::endl;
  }
  else if (strcmp(nargs[0], "rmpool") == 0) {
    if (nargs.size() < 2)
      usage();
    rados_pool_t rm_me;
    ret = rados.open_pool(nargs[1], &rm_me);
    if (ret >= 0) {
      ret = rados.delete_pool(rm_me);
      if (ret < 0) {
	cerr << "error deleting pool " << nargs[1] << ": "
	     << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      }
      cout << "successfully deleted pool " << nargs[1] << std::endl;
    } else { //error
      cerr << "pool " << nargs[1] << " does not exist" << std::endl;
    }
  }
  else if (strcmp(nargs[0], "lssnap") == 0) {
    if (!pool || nargs.size() != 1)
      usage();

    vector<snap_t> snaps;
    rados.snap_list(p, &snaps);
    for (vector<snap_t>::iterator i = snaps.begin();
	 i != snaps.end();
	 i++) {
      string s;
      time_t t;
      if (rados.snap_get_name(p, *i, &s) < 0)
	continue;
      if (rados.snap_get_stamp(p, *i, &t) < 0)
	continue;
      struct tm bdt;
      localtime_r(&t, &bdt);
      cout << *i << "\t" << s << "\t";

      cout.setf(std::ios::right);
      cout.fill('0');
      cout << std::setw(4) << (bdt.tm_year+1900)
	   << '.' << std::setw(2) << (bdt.tm_mon+1)
	   << '.' << std::setw(2) << bdt.tm_mday
	   << ' '
	   << std::setw(2) << bdt.tm_hour
	   << ':' << std::setw(2) << bdt.tm_min
	   << ':' << std::setw(2) << bdt.tm_sec
	   << std::endl;
      cout.unsetf(std::ios::right);
    }
    cout << snaps.size() << " snaps" << std::endl;
  }

  else if (strcmp(nargs[0], "mksnap") == 0) {
    if (!pool || nargs.size() < 2)
      usage();
    
    ret = rados.snap_create(p, nargs[1]);
    if (ret < 0) {
      cerr << "error creating pool " << pool << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    cout << "created pool " << pool << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rmsnap") == 0) {
    if (!pool || nargs.size() < 2)
      usage();
    
    ret = rados.snap_remove(p, nargs[1]);
    if (ret < 0) {
      cerr << "error removing pool " << pool << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    cout << "removed pool " << pool << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rollback") == 0) {
    if (!pool || nargs.size() < 3)
      usage();

    ret = rados.snap_rollback_object(p, nargs[1], nargs[2]);
    if (ret < 0) {
      cerr << "error rolling back pool " << pool << " to snapshot " << nargs[1] 
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      goto out;
    }
    cout << "rolled back pool " << pool
	 << " to snapshot " << nargs[2] << std::endl;
  }
  
  else if (strcmp(nargs[0], "bench") == 0) {
    if (!pool || nargs.size() < 3)
      usage();
    int seconds = atoi(nargs[1]);
    int operation = 0;
    if (strcmp(nargs[2], "write") == 0)
      operation = OP_WRITE;
    else if (strcmp(nargs[2], "seq") == 0)
      operation = OP_SEQ_READ;
    else if (strcmp(nargs[2], "rand") == 0)
      operation = OP_RAND_READ;
    else
      usage();
    ret = aio_bench(rados, p, operation, seconds, concurrent_ios, op_size);
    if (ret != 0)
      cerr << "error during benchmark: " << ret << std::endl;
  }
  else {
    cerr << "unrecognized command " << nargs[0] << std::endl;
    usage();
  }

 out:
  if (pool)
    rados.close_pool(p);

 no_pool_out:
  rados.shutdown();
  if (ret < 0)
    return 1;
  return 0;
}

