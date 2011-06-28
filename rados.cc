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
#include "global/global_init.h"
#include "common/Cond.h"
#include "common/debug.h"
#include "mds/inode_backtrace.h"
#include "auth/Crypto.h"
#include <iostream>
#include <fstream>

#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <errno.h>
#include <dirent.h>

int rados_tool_sync(const std::map < std::string, std::string > &opts,
                             std::vector<const char*> &args);

void usage()
{
  cerr << \
"usage: rados [options] [commands]\n"
"POOL COMMANDS\n"
"   lspools                         list pools\n"
"   mkpool <pool-name> [123[ 4]]     create pool <pool-name>'\n"
"                                    [with auid 123[and using crush rule 4]]\n"
"   rmpool <pool-name>               remove pool <pool-name>'\n"
"   mkpool <pool-name>               create the pool <pool-name>\n"
"   df                              show per-pool and total usage\n"
"   ls                               list objects in pool\n\n"
"   chown 123                        change the pool owner to auid 123\n"
"OBJECT COMMANDS\n"
"   get <obj-name> [outfile]         fetch object\n"
"   put <obj-name> [infile]          write object\n"
"   create <obj-name>                create object\n"
"   rm <obj-name>                    remove object\n"
"   listxattr <obj-name>\n"
"   getxattr <obj-name> attr\n"
"   setxattr <obj-name> attr val\n"
"   rmxattr <obj-name> attr\n"
"   stat objname                     stat the named object\n"
"   mapext <obj-name>\n"
"   lssnap                           list snaps\n"
"   mksnap <snap-name>               create snap <snap-name>\n"
"   rmsnap <snap-name>               remove snap <snap-name>\n"
"   rollback <obj-name> <snap-name>  roll back object to snap <snap-name>\n\n"
"   bench <seconds> write|seq|rand [-t concurrent_operations]\n"
"                                    default is 16 concurrent IOs and 4 MB op size\n\n"
"IMPORT AND EXPORT\n"
"   import [options] <local-directory> <rados-pool>\n"
"       Upload <local-directory> to <rados-pool>\n"
"   export [options] rados-pool> <local-directory>\n"
"       Download <rados-pool> to <local-directory>\n"
"   options:\n"
"       -f / --force                 Copy everything, even if it hasn't changed.\n"
"       -d / --delete-after          After synchronizing, delete unreferenced\n"
"                                    files or objects from the target bucket\n"
"                                    or directory.\n"
"GLOBAL OPTIONS:\n"
"   -p pool\n"
"   --pool=pool\n"
"        select given pool by name\n"
"   -b op_size\n"
"        set the size of write ops for put or benchmarking"
"   -s name\n"
"   --snap name\n"
"        select given snap name for (read) IO\n"
"   -i infile\n"
"   -o outfile\n"
"        specify input or output file (for certain commands)\n"
"   --create\n"
"        create the pool or directory that was specified\n";
}

static int do_get(IoCtx& io_ctx, const char *objname, const char *outfile, bool check_stdio)
{
  string oid(objname);
  bufferlist outdata;
  int ret = io_ctx.read(oid, outdata, 0, 0);
  if (ret < 0) {
    return ret;
  }

  if (check_stdio && strcmp(outfile, "-") == 0) {
    fwrite(outdata.c_str(), outdata.length(), 1, stdout);
  } else {
    outdata.write_file(outfile);
    generic_dout(0) << "wrote " << outdata.length() << " byte payload to " << outfile << dendl;
  }

  return 0;
}

static int do_put(IoCtx& io_ctx, const char *objname, const char *infile, int op_size, bool check_stdio)
{
  string oid(objname);
  bufferlist indata;
  bool stdio = false;
  if (check_stdio && strcmp(infile, "-") == 0)
    stdio = true;

  if (stdio) {
    char buf[256];
    while(!cin.eof()) {
      cin.getline(buf, 256);
      indata.append(buf);
      indata.append('\n');
    }
  } else {
    int ret, fd = open(infile, O_RDONLY);
    if (fd < 0) {
      char buf[80];
      cerr << "error reading input file " << infile << ": " << strerror_r(errno, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    char *buf = new char[op_size];
    int count = op_size;
    uint64_t offset = 0;
    while (count == op_size) {
      count = read(fd, buf, op_size);
      if (count == 0) {
        if (!offset) {
          int ret = io_ctx.create(oid, true);
          if (ret < 0)
            cerr << "WARNING: could not create object: " << oid << std::endl;
        }
        continue;
      }
      indata.append(buf, count);
      if (offset == 0)
	ret = io_ctx.write_full(oid, indata);
      else
	ret = io_ctx.write(oid, indata, count, offset);
      indata.clear();

      if (ret < 0) {
        close(fd);
        return ret;
      }
      offset += count;
    }
    close(fd);
  }
  return 0;
}

class RadosWatchCtx : public librados::WatchCtx {
  string name;
public:
  RadosWatchCtx(const char *imgname) : name(imgname) {}
  virtual ~RadosWatchCtx() {}
  virtual void notify(uint8_t opcode, uint64_t ver, bufferlist& bl) {
    string s;
    try {
      bufferlist::iterator iter = bl.begin();
      ::decode(s, iter);
    } catch (buffer::error *err) {
      cout << "could not decode bufferlist, buffer length=" << bl.length() << std::endl;
    }
    cout << name << " got notification opcode=" << (int)opcode << " ver=" << ver << " msg='" << s << "'" << std::endl;
  }
};

static const char alphanum_table[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int gen_rand_alphanumeric(char *dest, int size) /* size should be the required string size + 1 */
{
  int ret = get_random_bytes(dest, size);
  if (ret < 0) {
    cerr << "cannot get random bytes: " << strerror(-ret) << std::endl;
    return -1;
  }

  int i;
  for (i=0; i<size - 1; i++) {
    int pos = (unsigned)dest[i];
    dest[i] = alphanum_table[pos & 63];
  }
  dest[i] = '\0';

  return 0;
}

struct obj_info {
  string name;
  size_t len;
};

uint64_t get_random(uint64_t min_val, uint64_t max_val)
{
  uint64_t r;
  get_random_bytes((char *)&r, sizeof(r));
  r = min_val + r % (max_val - min_val + 1);
  return r;
}

class LoadGen {
  int read_write_ratio;
  size_t min_obj_len;
  size_t max_obj_len;
  size_t min_op_len;
  size_t max_op_len;
  size_t target_throughput;
  size_t total_transfered;
  size_t pending;
  int num_objs;

  IoCtx io_ctx;
  Rados *rados;

  map<int, obj_info> objs;

  utime_t start_time;

  enum {
    OP_READ,
    OP_WRITE,
  };

  struct LoadGenOp {
    int id;
    int op;
    string oid;
    size_t off;
    size_t len;
    LoadGen *lg;

    LoadGenOp() {}
    LoadGenOp(LoadGen *_lg) : lg(_lg) {}
  };

  int max_op;

  map<int, LoadGenOp> pending_ops;

  void gen_op(int& op_type, string& oid, size_t& off, size_t& len);
  void gen_next_op();

  uint64_t cur_rate() {
    utime_t now = ceph_clock_now(g_ceph_context);
    now -= start_time;
    uint64_t ns = now.nsec();
    float delta = ns / 1000000000;
    delta += now.sec();

    if (delta == 0)
      return 0;

    return total_transferred / delta;    
  }

  Mutex lock;

  void operate(LoadGenOp& op);


public:
  LoadGen(Rados *_rados) : rados(_rados), lock("LoadGen") {
    read_write_ratio = 4;
    min_obj_len = 1024;
    max_obj_len = (uint64_t)5 * 1024 * 1024 * 1024;
    min_op_len = 1024;
    max_op_len = 2 * 1024 * 1024;
    target_throughput = 5 * 1024 * 1024; // B/sec
    total_transfered = 0;
    pending = 0;
    num_objs = 1000;
    max_op = 0;
  }
  int bootstrap(const char *pool);
  int run();
  void cleanup();

  void io_cb(completion_t c) {
    Mutex::Locker l(lock);
    
  }
};

static void _load_gen_cb(completion_t c, void *param)
{
  LoadGen *lg = (LoadGen *)lg;
  lg->io_cb(c);
}

int LoadGen::bootstrap(const char *pool)
{
  char buf[128];
  int i;

  if (!pool) {
    cerr << "ERROR: pool name was not specified" << std::endl;
    return -EINVAL;
  }

  int ret = rados->ioctx_create(pool, io_ctx);
  if (ret < 0) {
    cerr << "error opening pool " << pool << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
    return ret;
  }

  int buf_len = 1;
  bufferptr p = buffer::create(buf_len);
  bufferlist bl;
  memset(p.c_str(), 0, buf_len);
  bl.push_back(p);

  vector<librados::AioCompletion *> completions;
  for (i = 0; i < num_objs; i++) {
    obj_info info;
    gen_rand_alphanumeric(buf, 16);
    info.name = "obj-";
    info.name.append(buf);
    info.len = get_random(min_obj_len, max_obj_len);

    librados::AioCompletion *c = rados->aio_create_completion(NULL, NULL, NULL);
    completions.push_back(c);
    // generate object
    ret = io_ctx.aio_write(info.name, c, bl, buf_len, info.len - buf_len);
    if (ret < 0) {
      cerr << "couldn't write obj: " << info.name << " ret=" << ret << std::endl;
      return ret;
    }
    objs[i] = info;
  }

  vector<librados::AioCompletion *>::iterator iter;
  for (iter = completions.begin(); iter != completions.end(); ++iter) {
    AioCompletion *c = *iter;
    c->wait_for_complete();
    ret = c->get_return_value();
    c->release();
    if (ret < 0) {
      cerr << "aio_write failed" << std::endl;
      return ret;
    }
  }
  return 0;
}

void operate(LoadGenOp& op)
{
  librados::AioCompletion *c = rados->aio_create_completion(NULL, NULL, NULL);
  int ret;

  switch (op.type) {
  case OP_READ:
    ret = io_ctx.aio_read(op.oid, c, &op.bl, op.len, op.off);
    break;
  case OP_WRITE:
    bufferptr p = buffer::create(op.len);
    memset(p.c_str(), 0, op.len);
    op.bl.push_back(p);
    
    ret = io_ctx.aio_write(op.oid, c, op.bl, op.len, op.off);
    break;
  }
}

void LoadGen::gen_op(LoadGenOp& op)
{
  int i = get_random(0, objs.size() - 1);
  obj_info& info = objs[i];
  op.oid = info.name;

  size_t len = get_random(min_op_len, max_op_len);
  if (len > info.len)
    len = info.len;
  size_t off = get_random(0, info.len);

  if (off + len > info.len)
    off = info.len - len;

  op.off = off;
  op.len = len;

  i = get_random(0, read_write_ratio + 1);
  if (i == 0)
    op.type = OP_WRITE;
  else
    op.type = OP_READ;
}

uint64_t LoadGen::gen_next_op()
{
  Mutex::Locker l(lock);

  LoadGenOp op(this);
  gen_op(op);
  op.id = max_op++;
  ops[op.id] = op;
  cout << (op_type == OP_READ ? "READ" : "WRITE") << " : oid=" << oid << " off=" << off << " len=" << len << std::endl;
  operate(op);

  return op.len;
}

int LoadGen::run()
{
  start_time = ceph_clock_now(g_ceph_context);

  cout << "warmup" << std::endl;
  // warmup
  for (int i = 0; i < 100; i++) {
    gen_next_op();
  }

  while (1) {
    usleep(1000);
  }

  

  return 0;
}

void LoadGen::cleanup()
{
  cout << "cleaning up objects" << std::endl;
  map<int, obj_info>::iterator iter;
  for (iter = objs.begin(); iter != objs.end(); ++iter) {
    obj_info& info = iter->second;
    int ret = io_ctx.remove(info.name);
    if (ret < 0)
      cerr << "couldn't remove obj: " << info.name << " ret=" << ret << std::endl;
  }
}

/**********************************************

**********************************************/
static int rados_tool_common(const std::map < std::string, std::string > &opts,
                             std::vector<const char*> &nargs)
{
  int ret;
  bool create_pool = false;
  const char *pool_name = NULL;
  int concurrent_ios = 16;
  int op_size = 1 << 22;
  const char *snapname = NULL;
  snap_t snapid = CEPH_NOSNAP;
  std::map<std::string, std::string>::const_iterator i;

  i = opts.find("create");
  if (i != opts.end()) {
    create_pool = true;
  }
  i = opts.find("pool");
  if (i != opts.end()) {
    pool_name = i->second.c_str();
  }
  i = opts.find("concurrent-ios");
  if (i != opts.end()) {
    concurrent_ios = strtol(i->second.c_str(), NULL, 10);
  }
  i = opts.find("block-size");
  if (i != opts.end()) {
    op_size = strtol(i->second.c_str(), NULL, 10);
  }
  i = opts.find("snap");
  if (i != opts.end()) {
    snapname = i->second.c_str();
  }
  i = opts.find("snapid");
  if (i != opts.end()) {
    snapid = strtoll(i->second.c_str(), NULL, 10);
  }

  // open rados
  Rados rados;
  ret = rados.init_with_context(g_ceph_context);
  if (ret) {
     cerr << "couldn't initialize rados! error " << ret << std::endl;
     return ret;
  }

  ret = rados.connect();
  if (ret) {
     cerr << "couldn't connect to cluster! error " << ret << std::endl;
     return ret;
  }
  char buf[80];

  if (create_pool && !pool_name) {
    cerr << "--create-pool requested but pool_name was not specified!" << std::endl;
    usage();
  }

  if (create_pool) {
    ret = rados.pool_create(pool_name, 0, 0);
    if (ret < 0) {
      cerr << "error creating pool " << pool_name << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  // open io context.
  IoCtx io_ctx;
  if (pool_name) {
    ret = rados.ioctx_create(pool_name, io_ctx);
    if (ret < 0) {
      cerr << "error opening pool " << pool_name << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  // snapname?
  if (snapname) {
    ret = io_ctx.snap_lookup(snapname, &snapid);
    if (ret < 0) {
      cerr << "error looking up snap '" << snapname << "': " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  if (snapid != CEPH_NOSNAP) {
    string name;
    ret = io_ctx.snap_get_name(snapid, &name);
    if (ret < 0) {
      cerr << "snapid " << snapid << " doesn't exist in pool "
	   << io_ctx.get_pool_name() << std::endl;
      return 1;
    }
    io_ctx.snap_set_read(snapid);
    cout << "selected snap " << snapid << " '" << snapname << "'" << std::endl;
  }

  assert(!nargs.empty());

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
    rados.get_pool_stats(vec, stats);

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
    cluster_stat_t tstats;
    rados.cluster_stat(tstats);
    printf("  total used    %12lld %12lld\n", (long long unsigned)tstats.kb_used,
	   (long long unsigned)tstats.num_objects);
    printf("  total avail   %12lld\n", (long long unsigned)tstats.kb_avail);
    printf("  total space   %12lld\n", (long long unsigned)tstats.kb);
  }

  else if (strcmp(nargs[0], "ls") == 0) {
    if (!pool_name) {
      cerr << "pool name was not specified" << std::endl;
      return 1;
    }

    bool stdout = (nargs.size() < 2) || (strcmp(nargs[1], "-") == 0);
    ostream *outstream;
    if(stdout)
      outstream = &cout;
    else
      outstream = new ofstream(nargs[1]);

    {
      librados::ObjectIterator i = io_ctx.objects_begin();
      librados::ObjectIterator i_end = io_ctx.objects_end();
      for (; i != i_end; ++i) {
	*outstream << *i << std::endl;
      }
    }
    if (!stdout)
      delete outstream;
  }
  else if (strcmp(nargs[0], "chown") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();

    uint64_t new_auid = strtol(nargs[1], 0, 10);
    ret = io_ctx.set_auid(new_auid);
    if (ret < 0) {
      cerr << "error changing auid on pool " << io_ctx.get_pool_name() << ':'
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
    } else cerr << "changed auid on pool " << io_ctx.get_pool_name()
		<< " to " << new_auid << std::endl;
  }
  else if (strcmp(nargs[0], "mapext") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    std::map<uint64_t,uint64_t> m;
    ret = io_ctx.mapext(oid, 0, -1, m);
    if (ret < 0) {
      cerr << "mapext error on " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    std::map<uint64_t,uint64_t>::iterator iter;
    for (iter = m.begin(); iter != m.end(); ++iter) {
      cout << hex << iter->first << "\t" << iter->second << dec << std::endl;
    }
  }
  else if (strcmp(nargs[0], "stat") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    uint64_t size;
    time_t mtime;
    ret = io_ctx.stat(oid, &size, &mtime);
    if (ret < 0) {
      cerr << " error stat-ing " << pool_name << "/" << oid << ": "
           << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    } else {
      cout << pool_name << "/" << oid
           << " mtime " << mtime << ", size " << size << std::endl;
    }
  }
  else if (strcmp(nargs[0], "get") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();
    ret = do_get(io_ctx, nargs[1], nargs[2], true);
    if (ret < 0) {
      cerr << "error getting " << pool_name << "/" << nargs[1] << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "put") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();
    ret = do_put(io_ctx, nargs[1], nargs[2], op_size, true);
    if (ret < 0) {
      cerr << "error putting " << pool_name << "/" << nargs[1] << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "setxattr") == 0) {
    if (!pool_name || nargs.size() < 4)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);
    string attr_val(nargs[3]);

    bufferlist bl;
    bl.append(attr_val.c_str(), attr_val.length());

    ret = io_ctx.setxattr(oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error setting xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    else
      ret = 0;
  }
  else if (strcmp(nargs[0], "getxattr") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    bufferlist bl;
    ret = io_ctx.getxattr(oid, attr_name.c_str(), bl);
    if (ret < 0) {
      cerr << "error getting xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    else
      ret = 0;
    string s(bl.c_str(), bl.length());
    cout << s << std::endl;
  } else if (strcmp(nargs[0], "rmxattr") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();

    string oid(nargs[1]);
    string attr_name(nargs[2]);

    ret = io_ctx.rmxattr(oid, attr_name.c_str());
    if (ret < 0) {
      cerr << "error removing xattr " << pool_name << "/" << oid << "/" << attr_name << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  } else if (strcmp(nargs[0], "listxattr") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();

    string oid(nargs[1]);
    map<std::string, bufferlist> attrset;
    bufferlist bl;
    ret = io_ctx.getxattrs(oid, attrset);
    if (ret < 0) {
      cerr << "error getting xattr set " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }

    for (map<std::string, bufferlist>::iterator iter = attrset.begin();
         iter != attrset.end(); ++iter) {
      cout << iter->first << std::endl;
    }
  }
  else if (strcmp(nargs[0], "rm") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    ret = io_ctx.remove(oid);
    if (ret < 0) {
      cerr << "error removing " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }
  else if (strcmp(nargs[0], "create") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    ret = io_ctx.create(oid, true);
    if (ret < 0) {
      cerr << "error creating " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
  }

  else if (strcmp(nargs[0], "tmap") == 0) {
    if (nargs.size() < 3)
      usage();
    if (strcmp(nargs[1], "dump") == 0) {
      bufferlist outdata;
      string oid(nargs[2]);
      ret = io_ctx.read(oid, outdata, 0, 0);
      if (ret < 0) {
	cerr << "error reading " << pool_name << "/" << oid << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
	return 1;
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
    else if (strcmp(nargs[1], "set") == 0 ||
	     strcmp(nargs[1], "create") == 0) {
      if (nargs.size() < 5)
	usage();
      string oid(nargs[2]);
      string k(nargs[3]);
      string v(nargs[4]);
      bufferlist bl;
      char c = (strcmp(nargs[1], "set") == 0) ? CEPH_OSD_TMAP_SET : CEPH_OSD_TMAP_CREATE;
      ::encode(c, bl);
      ::encode(k, bl);
      ::encode(v, bl);
      ret = io_ctx.tmap_update(oid, bl);
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
    ret = rados.pool_create(nargs[1], auid, crush_rule);
    if (ret < 0) {
      cerr << "error creating pool " << nargs[1] << ": "
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "successfully created pool " << nargs[1] << std::endl;
  }
  else if (strcmp(nargs[0], "rmpool") == 0) {
    if (nargs.size() < 2)
      usage();
    ret = rados.pool_delete(nargs[1]);
    if (ret >= 0) {
      cout << "successfully deleted pool " << nargs[1] << std::endl;
    } else { //error
      cerr << "pool " << nargs[1] << " does not exist" << std::endl;
    }
  }
  else if (strcmp(nargs[0], "lssnap") == 0) {
    if (!pool_name || nargs.size() != 1)
      usage();

    vector<snap_t> snaps;
    io_ctx.snap_list(&snaps);
    for (vector<snap_t>::iterator i = snaps.begin();
	 i != snaps.end();
	 i++) {
      string s;
      time_t t;
      if (io_ctx.snap_get_name(*i, &s) < 0)
	continue;
      if (io_ctx.snap_get_stamp(*i, &t) < 0)
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
    if (!pool_name || nargs.size() < 2)
      usage();

    ret = io_ctx.snap_create(nargs[1]);
    if (ret < 0) {
      cerr << "error creating pool " << pool_name << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "created pool " << pool_name << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rmsnap") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();

    ret = io_ctx.snap_remove(nargs[1]);
    if (ret < 0) {
      cerr << "error removing pool " << pool_name << " snapshot " << nargs[1]
	   << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "removed pool " << pool_name << " snap " << nargs[1] << std::endl;
  }

  else if (strcmp(nargs[0], "rollback") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();

    ret = io_ctx.rollback(nargs[1], nargs[2]);
    if (ret < 0) {
      cerr << "error rolling back pool " << pool_name << " to snapshot " << nargs[1]
	   << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
      return 1;
    }
    cout << "rolled back pool " << pool_name
	 << " to snapshot " << nargs[2] << std::endl;
  }
  else if (strcmp(nargs[0], "bench") == 0) {
    if (!pool_name || nargs.size() < 3)
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
    ret = aio_bench(rados, io_ctx, operation, seconds, concurrent_ios, op_size);
    if (ret != 0)
      cerr << "error during benchmark: " << ret << std::endl;
  }
  else if (strcmp(nargs[0], "watch") == 0) {
    if (!pool_name || nargs.size() < 2)
      usage();
    string oid(nargs[1]);
    RadosWatchCtx ctx(oid.c_str());
    uint64_t cookie;
    ret = io_ctx.watch(oid, 0, &cookie, &ctx);
    if (ret != 0)
      cerr << "error calling watch: " << ret << std::endl;
    else {
      cout << "press enter to exit..." << std::endl;
      getchar();
    }
  }
  else if (strcmp(nargs[0], "notify") == 0) {
    if (!pool_name || nargs.size() < 3)
      usage();
    string oid(nargs[1]);
    string msg(nargs[2]);
    bufferlist bl;
    ::encode(msg, bl);
    ret = io_ctx.notify(oid, 0, bl);
    if (ret != 0)
      cerr << "error calling notify: " << ret << std::endl;
  } else if (strcmp(nargs[0], "load-gen") == 0) {
    if (!pool_name)
      usage();
    LoadGen lg(&rados);
    cout << "preparing objects" << std::endl;
    ret = lg.bootstrap(pool_name);
    if (ret < 0) {
      cerr << "load-gen bootstrap failed" << std::endl;
      exit(1);
    }
    lg.run();
    lg.cleanup();
  }  else {
    cerr << "unrecognized command " << nargs[0] << std::endl;
    usage();
  }

  if (ret)
    cerr << "error " << (-ret) << ": " << strerror_r(-ret, buf, sizeof(buf)) << std::endl;
  return (ret < 0) ? 1 : 0;
}

int main(int argc, const char **argv)
{
  DEFINE_CONF_VARS(usage);
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  env_to_vec(args);

  global_init(args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  std::map < std::string, std::string > opts;
  std::vector<const char*>::iterator i;
  std::string val;
  for (i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage();
      exit(0);
    } else if (ceph_argparse_flag(args, i, "-f", "--force", (char*)NULL)) {
      opts["force"] = "true";
    } else if (ceph_argparse_flag(args, i, "-d", "--delete-after", (char*)NULL)) {
      opts["delete-after"] = "true";
    } else if (ceph_argparse_flag(args, i, "-C", "--create", "--create-pool",
				  (char*)NULL)) {
      opts["create"] = "true";
    } else if (ceph_argparse_witharg(args, i, &val, "-p", "--pool", (char*)NULL)) {
      opts["pool"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-t", "--concurrent-ios", (char*)NULL)) {
      opts["concurrent-ios"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-s", "--snap", (char*)NULL)) {
      opts["snap"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-S", "--snapid", (char*)NULL)) {
      opts["snapid"] = val;
    } else {
      if (val[0] == '-')
        usage();
      i++;
    }
  }

  if (args.empty()) {
    cerr << "rados: you must give an action. Try --help" << std::endl;
    return 1;
  }
  if ((strcmp(args[0], "import") == 0) || (strcmp(args[0], "export") == 0))
    return rados_tool_sync(opts, args);
  else
    return rados_tool_common(opts, args);
}
