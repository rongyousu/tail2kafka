#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>
#include <sys/inotify.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <rdkafka.h>

// -llua-5.1 -lpthread -lrt -lz

static const char   NL = '\n';
static const int    UNSET_INT = INT_MAX;
static const size_t MAX_LINE_LEN = 10240;
static const size_t MAX_FILES    = 64;
static const size_t ONE_EVENT_SIZE =
  sizeof(struct inotify_event) + NAME_MAX;
static const size_t MAX_ERR_LEN = 512;

struct LuaCtx;

typedef std::map<std::string, int>         SIHash;
typedef std::map<std::string, SIHash>      SSIHash;
typedef std::vector<std::string>           StringList;
typedef std::vector<std::string *>         StringPtrList;
typedef std::vector<int>                   IntList;
typedef std::vector<LuaCtx *>              LuaCtxPtrList;
typedef std::map<int, LuaCtx *>            WatchCtxHash;
typedef std::map<std::string, std::string> SSHash;
typedef std::vector<rd_kafka_topic_t *>    TopicList;

struct CnfCtx {
  std::string host;
  std::string brokers;
  SSHash      kafkaGlobal;
  SSHash      kafkaTopic;

  rd_kafka_conf_t       *conf;
  rd_kafka_topic_conf_t *tconf;
  rd_kafka_t            *rk;
  TopicList              rkts;

  LuaCtxPtrList          luaCtxs;
  WatchCtxHash           wch;
  int                    wfd;
  int                    accept;
  int                    server;
  uint64_t               sn;
  
  CnfCtx() {
    conf  = 0;
    tconf = 0;
    rk    = 0;
    sn    = 0;
  }
};

typedef bool (*transform_pt)(
  LuaCtx *ctx, const char *line, size_t nline,
  std::string *result);
bool transform(LuaCtx *ctx, const char *line, size_t nline, std::string *result);

typedef bool(*grep_pt)(
  LuaCtx *ctx, const StringList &fields,
  std::string *result);
bool grep(LuaCtx *ctx, const StringList &fields, std::string *result);

typedef bool (*aggregate_pt)(
  LuaCtx *ctx, const StringList &fields,
  StringPtrList *results);
bool aggregate(LuaCtx *ctx, const StringList &fields, StringPtrList *results);

typedef bool (*filter_pt)(
  LuaCtx *ctx, const StringList &fields,
  std::string *result);
bool filter(LuaCtx *ctx, const StringList &fields, std::string *result);

struct LuaCtx {
  int           idx;
  
  CnfCtx       *main;
  lua_State    *L;
  
  int           fd;
  ino_t         inode;
  std::string   file;
  off_t         size;
  std::string   topic;

  bool          autosplit;
  bool          withhost;
  bool          withtime;
  int           timeidx;

  transform_pt  transform;
  grep_pt       grep;

  std::string   lasttime;
  SSIHash       cache;
  aggregate_pt  aggregate;

  IntList       filters;
  filter_pt     filter;

  char         *buffer;
  size_t        npos;

  uint64_t      sn;

  LuaCtx();
};

LuaCtx::LuaCtx()
{
  L  = 0;
  fd = -1;

  autosplit = false;
  withhost  = true;
  withtime  = true;
  timeidx   = UNSET_INT;
  
  buffer = new char[MAX_LINE_LEN];
  npos = 0;

  transform = 0;
  grep      = 0;
  aggregate = 0;
  filter    = 0;

  sn = 0;
}

struct OneTaskReq {
  int            idx;
  StringPtrList *datas;
};

CnfCtx *loadCnf(const char *dir, char *errbuf);
void unloadCnfCtx(CnfCtx *ctx);
pid_t spawn(CnfCtx *ctx, char *errbuf);

enum Want {WAIT, START, RELOAD, STOP} want;

#ifndef UNITTEST
int main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(stderr, "%s confdir\n", argv[1]);
    return EXIT_FAILURE;
  }

  const char *dir = argv[1];

  pid_t pid;
  char errbuf[MAX_ERR_LEN];

  CnfCtx *ctx = loadCnf(dir, errbuf);
  if (!ctx) {
    fprintf(stderr, "load cnf error %s\n", errbuf);
  }
  want = START;

  while (true) {
    if (want == START) {
      pid = spawn(ctx, errbuf);
      if (pid == -1) {
        fprintf(stderr, "spawn failed, exit\n");
        break;
      }
    } else if (want == STOP) {
      kill(pid, SIGUSR1);
      break;
    } else if (want == RELOAD) {
      CnfCtx *nctx = loadCnf(dir, errbuf);
      if (nctx) {
        pid_t npid = spawn(nctx, errbuf);
        if (npid != -1) {
          kill(pid, SIGUSR2);
          unloadCnfCtx(ctx);
          ctx = nctx;
          pid = npid;
        } else {
          unloadCnfCtx(nctx);
        }
      } else {
        fprintf(stderr, "load cnf error %s\n", errbuf);
      }
    }
    want = WAIT;

    int status;
    if (wait(&status) != -1) {
      if (WIFSIGNALED(status) && WTERMSIG(status) != SIGUSR2) {
        want = START;
      }
    }
  }
  
  unloadCnfCtx(ctx);
  return EXIT_SUCCESS;
}
#endif

void unloadLuaCtx(LuaCtx *ctx)
{
  if (ctx->L) lua_close(ctx->L);
  if (ctx->fd != -1) close(ctx->fd);
  if (ctx->buffer) delete[] ctx->buffer;
  delete ctx;
}

static LuaCtx *loadLuaCtx(const char *file, char *errbuf)
{
  int size;
  LuaCtx *ctx = new LuaCtx;
  
  lua_State *L;
  ctx->L = L = luaL_newstate();
  luaL_openlibs(L);
  if (luaL_dofile(L, file) != 0) {
    snprintf(errbuf, MAX_ERR_LEN, "load %s error\n%s", file, lua_tostring(L, 1));
    goto error;
  }

  lua_getglobal(L, "file");
  if (!lua_isstring(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s file must be string", file);
    goto error;
  }

  ctx->file = lua_tostring(L, 1);
  struct stat st;
  if (stat(ctx->file.c_str(), &st) == -1) {
    snprintf(errbuf, MAX_ERR_LEN, "%s file %s stat failed", file, ctx->file.c_str());
    goto error;
  }
  lua_settop(L, 0);

  lua_getglobal(L, "topic");
  if (!lua_isstring(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s topic must be string", file);
    goto error;
  }
  ctx->topic = lua_tostring(L, 1);
  lua_settop(L, 0);

  lua_getglobal(L, "autosplit");
  if (!lua_isnil(L, 1)) {
    if (!lua_isboolean(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s autosplit must be boolean", file);
      goto error;
    }
    ctx->autosplit = lua_toboolean(L, 1);
  }
  lua_settop(L, 0);

  lua_getglobal(L, "timeidx");
  if (!lua_isnil(L, 1)) {
    if (!lua_isnumber(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s timeidx must be number", file);
      goto error;
    }
    ctx->timeidx = lua_tonumber(L, 1);
  }
  lua_settop(L, 0);

  lua_getglobal(L, "withtime");
  if (!lua_isnil(L, 1)) {
    if (!lua_isboolean(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s withtime must be boolean", file);
      goto error;
    }
    ctx->withtime = lua_toboolean(L, 1);
  }
  lua_settop(L, 0);

  lua_getglobal(L, "withhost");
  if (!lua_isnil(L, 1)) {
    if (!lua_isboolean(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s withhost must be boolean", file);
      goto error;
    }
    ctx->withhost = lua_toboolean(L, 1);
  }
  lua_settop(L, 0);
  
  lua_getglobal(L, "filter");
  if (!lua_isnil(L, 1)) {
    if (!lua_istable(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s filter must be table", file);
      goto error;
    }
    
    size = luaL_getn(L, 1);
    if (size == 0) {
      snprintf(errbuf, MAX_ERR_LEN, "%s filter element number must >0", file);
      goto error;
    }
    
    for (int i = 0; i < size; ++i) {
      lua_pushinteger(L, i+1);
      lua_gettable(L, 1);
      if (!lua_isnumber(L, -1)) {
        snprintf(errbuf, MAX_ERR_LEN, "%s filter element must be number", file);
        goto error;
      }
      ctx->filters.push_back(lua_tonumber(L, -1));
    }
    ctx->filter = filter;
  }
  lua_settop(L, 0);

  lua_getglobal(L, "aggregate");
  if (!lua_isnil(L, 1)) {
    if (!lua_isfunction(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s aggregate must be function", file);
      goto error;
    }
    ctx->aggregate = aggregate;
    if (ctx->timeidx == UNSET_INT) {
      snprintf(errbuf, MAX_ERR_LEN, "%s aggreagte must have timeidx", file);
      goto error;
    }
  }
  lua_settop(L, 0);

  lua_getglobal(L, "grep");
  if (!lua_isnil(L, 1)) {
    if (!lua_isfunction(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s grep must be function", file);
      goto error;
    }
    ctx->grep = grep;
  }
  lua_settop(L, 0);

  lua_getglobal(L, "transform");
  if (!lua_isnil(L, 1)) {
    if (!lua_isfunction(L, 1)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s transform must be function", file);
      goto error;
    }
    ctx->transform = transform;
  }
  lua_settop(L, 0);

  return ctx;
 error:
  unloadLuaCtx(ctx);
  return 0;
}

void uninitKafka(CnfCtx *ctx);

void unloadCnfCtx(CnfCtx *ctx)
{
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin();
       ite != ctx->luaCtxs.end(); ++ite) {
    unloadLuaCtx(*ite);
  }
  uninitKafka(ctx);
  delete ctx;
}

bool shell(const char *cmd, std::string *output, char *errbuf)
{
  FILE *fp = popen(cmd, "r");
  if (!fp) {
    snprintf(errbuf, MAX_ERR_LEN, "%s exec error", cmd);
    return false;
  }

  char buf[256];
  while (fgets(buf, 256, fp)) {
    output->append(buf);
  }

  int status = pclose(fp);
  if (status != 0) {
    snprintf(errbuf, MAX_ERR_LEN, "%s exit %d", cmd, status);
    return false;
  }

  return true;
}  

CnfCtx *loadCnfCtx(const char *file, char *errbuf)
{
  CnfCtx *ctx = new CnfCtx;

  lua_State *L;
  L = luaL_newstate();
  luaL_openlibs(L);
  if (luaL_dofile(L, file) != 0) {
    snprintf(errbuf, MAX_ERR_LEN, "load %s error\n%s", file, lua_tostring(L, 1));
    goto error;
  }

  lua_getglobal(L, "hostshell");
  if (!lua_isstring(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s hostshell must be string", file);
    goto error;
  }
  if (!shell(lua_tostring(L, 1), &ctx->host, errbuf)) {
    goto error;
  }
  lua_settop(L, 0);

  lua_getglobal(L, "brokers");
  if (!lua_isstring(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s borkers must be string", file);
    goto error;
  }
  ctx->brokers = lua_tostring(L, 1);
  lua_settop(L, 0);

  lua_getglobal(L, "kafka_global");
  if (!lua_istable(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s kafka_global must be hash table", file);
    goto error;
  }
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING) {
      snprintf(errbuf, MAX_ERR_LEN, "%s kafka_global key must be string", file);
      goto error;
    }
    if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER) {
      snprintf(errbuf, MAX_ERR_LEN, "%s kafka_global value must be string", file);
      goto error;
    }

    ctx->kafkaGlobal.insert(std::make_pair(lua_tostring(L, -2), lua_tostring(L, -1)));
    lua_pop(L, 1);
  }
  lua_settop(L, 0);

  lua_getglobal(L, "kafka_topic");
  if (!lua_istable(L, 1)) {
    snprintf(errbuf, MAX_ERR_LEN, "%s kafka_topic must be hash table", file);
    goto error;
  }
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING) {
      snprintf(errbuf, MAX_ERR_LEN, "%s kafka_topic key must be string", file);
      goto error;
    }
    if (lua_type(L, -1) != LUA_TSTRING && lua_type(L, -1) != LUA_TNUMBER) {
      snprintf(errbuf, MAX_ERR_LEN, "%s kafka_topic value must be string", file);
      goto error;
    }

    ctx->kafkaTopic.insert(std::make_pair(lua_tostring(L, -2), lua_tostring(L, -1)));
    lua_pop(L, 1);
  }
  lua_settop(L, 0);

  lua_close(L);
  return ctx;

  error:
  lua_close(L);
  unloadCnfCtx(ctx);
  return 0;
  
}

CnfCtx *loadCnf(const char *dir, char *errbuf)
{
  DIR *dh = opendir(dir);
  if (!dir) {
    snprintf(errbuf, MAX_ERR_LEN, "could not opendir %s", dir);
    return 0;
  }

  static const size_t N = 1024;
  char fullpath[N];

  snprintf(fullpath, N, "%s/main.lua", dir);

  CnfCtx *ctx = loadCnfCtx(fullpath, errbuf);
  if (!ctx) return 0;

  int i = 0;
  struct dirent *ent;
  while ((ent = readdir(dh))) {
    size_t len = strlen(ent->d_name);
    if (len <= 4 || ent->d_name[len-4] != '.' ||
        ent->d_name[len-3] != 'l' || ent->d_name[len-2] != 'u' ||
        ent->d_name[len-1] != 'a' || strcmp(ent->d_name, "main.lua") == 0) {
      continue;
    }

    snprintf(fullpath, N, "%s/%s", dir, ent->d_name);
    LuaCtx *lctx = loadLuaCtx(fullpath, errbuf);
    if (!lctx) {
      unloadCnfCtx(ctx);
      closedir(dh);
      return 0;
    }

    lctx->idx = i++;
    lctx->main = ctx;
    ctx->luaCtxs.push_back(lctx);
  }

  closedir(dh);

  int fd[2];
  if (pipe(fd) == -1) {
    snprintf(errbuf, MAX_ERR_LEN, "pipe error");
    unloadCnfCtx(ctx);
    return 0;
  }
  ctx->accept = fd[0];
  ctx->server = fd[1];
  
  return ctx;
}

bool transform(LuaCtx *ctx, const char *line, size_t nline, std::string *result)
{
  lua_getglobal(ctx->L, "transform");
  lua_pushlstring(ctx->L, line, nline);
  if (lua_pcall(ctx->L, 1, 1, 0) != 0) {
    fprintf(stderr, "%s transform error %s\n", ctx->file.c_str(), lua_tostring(ctx->L, -1));
    lua_settop(ctx->L, 0);
    return false;
  }

  if (lua_isnil(ctx->L, 1)) {
    result->clear();
    lua_settop(ctx->L, 0);
    return true;
  }
  
  if (!lua_isstring(ctx->L, 1)) {
    fprintf(stderr, "%s transform return #1 must be string(nil)\n", ctx->file.c_str());
    lua_settop(ctx->L, 0);
    return false;
  }

  if (ctx->withhost) result->assign(ctx->main->host).append(1, ' ');
  else result->clear();
  result->append(lua_tostring(ctx->L, 1));
  lua_settop(ctx->L, 0);
  return true;
}

bool grep(LuaCtx *ctx, const StringList &fields, std::string *result)
{
  lua_getglobal(ctx->L, "grep");

  lua_newtable(ctx->L);
  int table = lua_gettop(ctx->L);

  for (size_t i = 0; i < fields.size(); ++i) {
    lua_pushinteger(ctx->L, i+1);
    lua_pushstring(ctx->L, fields[i].c_str());
    lua_settable(ctx->L, table);
  }

  if (lua_pcall(ctx->L, 1, 1, 0) != 0) {
    fprintf(stderr, "%s grep error %s\n", ctx->file.c_str(), lua_tostring(ctx->L, -1));
    lua_settop(ctx->L, 0);
    return false;
  }

  if (lua_isnil(ctx->L, 1)) {
    lua_settop(ctx->L, 0);
    result->clear();
    return true;
  }

  if (!lua_istable(ctx->L, 1)) {
    fprintf(stderr, "%s grep return #1 must be table\n", ctx->file.c_str());
    lua_settop(ctx->L, 0);
    return false;
  }

  int size = luaL_getn(ctx->L, 1);
  if (size == 0) {
    fprintf(stderr, "%s grep return #1 must be not empty\n", ctx->file.c_str());
    lua_settop(ctx->L, 0);
    return false;
  }

  if (ctx->withhost) result->assign(ctx->main->host);
  else result->clear();

  for (int i = 0; i < size; ++i) {
    if (!result->empty()) result->append(1, ' ');
    lua_pushinteger(ctx->L, i+1);
    lua_gettable(ctx->L, 1);
    if (!lua_isstring(ctx->L, -1) && !lua_isnumber(ctx->L, -1)) {
      fprintf(stderr, "%s grep return #1[%d] is not string", ctx->file.c_str(), i);
      lua_settop(ctx->L, 0);
      return false;
    }
    result->append(lua_tostring(ctx->L, -1));
  }

  lua_settop(ctx->L, 0);
  return true;
}  

inline int absidx(int idx, size_t total)
{
  assert(total != 0);
  return idx > 0 ? idx-1 : total + idx;
}

inline std::string to_string(int i)
{
  std::string s;
  do {
    s.append(1, i%10 + '0');
    i /= 10;
  } while (i);
  std::reverse(s.begin(), s.end());
  return s;
}

void serializeCache(LuaCtx *ctx, StringPtrList *results)
{
  for (SSIHash::iterator ite = ctx->cache.begin(); ite != ctx->cache.end(); ++ite) {
    std::string *s = new std::string;
    if (ctx->withhost) s->append(ctx->main->host).append(1, ' ');
    if (ctx->withtime) s->append(ctx->lasttime).append(1, ' ');

    s->append(ite->first);
    for (SIHash::iterator jte = ite->second.begin(); jte != ite->second.end(); ++jte) {
      s->append(1, ' ').append(jte->first).append(1, '=').append(to_string(jte->second));
    }
    results->push_back(s);
  }
}

void flushCache(CnfCtx *ctx, bool timeout)
{
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin();
       ite != ctx->luaCtxs.end(); ++ite) {
    if ((*ite)->cache.empty()) continue;

    if (timeout || (*ite)->sn + 1000 < ctx->sn) {
      fprintf(stderr, "%s timeout flush cache\n", (*ite)->file.c_str());
      (*ite)->sn = ctx->sn;
      
      StringPtrList *datas = new StringPtrList;
      serializeCache(*ite, datas);
      OneTaskReq req = {(*ite)->idx, datas};
      ssize_t nn = write(ctx->server, &req, sizeof(OneTaskReq));
      assert(nn != -1 && nn == sizeof(OneTaskReq));
    }
  }
}  

bool aggregate(LuaCtx *ctx, const StringList &fields, StringPtrList *results)
{
  std::string curtime = fields[absidx(ctx->timeidx, fields.size())];
  if (!ctx->lasttime.empty() && curtime != ctx->lasttime) {
    serializeCache(ctx, results);
    ctx->cache.clear();
  }
  ctx->lasttime = curtime;
  
  lua_getglobal(ctx->L, "aggregate");

  lua_newtable(ctx->L);
  int table = lua_gettop(ctx->L);
  
  for (size_t i = 0; i < fields.size(); ++i) {
    lua_pushinteger(ctx->L, i+1);
    lua_pushstring(ctx->L, fields[i].c_str());
    lua_settable(ctx->L, table);
  }

  if (lua_pcall(ctx->L, 1, 2, 0) != 0) {
    fprintf(stderr, "%s aggregate error %s\n", ctx->file.c_str(), lua_tostring(ctx->L, -1));
    lua_settop(ctx->L, 0);
    return false;
  }

  if (!lua_isstring(ctx->L, 1)) {
    fprintf(stderr, "%s aggregate return #1 must be string\n", ctx->file.c_str());
    lua_settop(ctx->L, 0);
    return false;
  }
  std::string pkey = lua_tostring(ctx->L, 1);

  if (!lua_istable(ctx->L, 2)) {
    fprintf(stderr, "%s aggregate return #2 must be hash table\n", ctx->file.c_str());
    lua_settop(ctx->L, 0);
    return false;
  }
  lua_pushnil(ctx->L);  
  while (lua_next(ctx->L, 2) != 0) {
    if (lua_type(ctx->L, -2) != LUA_TSTRING) {
      fprintf(stderr, "%s aggregate return #3 key must be string\n", ctx->file.c_str());
      lua_settop(ctx->L, 0);
      return false;
    }
    if (lua_type(ctx->L, -1) != LUA_TNUMBER) {
      fprintf(stderr, "%s aggregate return #3 value must be number\n", ctx->file.c_str());
      lua_settop(ctx->L, 0);
      return false;
    }

    std::string key = lua_tostring(ctx->L, -2);
    int value = lua_tonumber(ctx->L, -1);

    ctx->cache[pkey][key] += value;
    lua_pop(ctx->L, 1);
  }

  lua_settop(ctx->L, 0);
  return true;
}

bool filter(LuaCtx *ctx, const StringList &fields, std::string *result)
{
  std::string s;

  if (ctx->withhost) result->assign(ctx->main->host);
  else result->clear();
  
  for (IntList::iterator ite = ctx->filters.begin();
       ite != ctx->filters.end(); ++ite) {
    int idx = absidx(*ite, fields.size());
    if (idx < 0 || (size_t) idx >= fields.size()) return false;

    if (!result->empty()) result->append(1, ' ');
    result->append(fields[idx]);
  }
  return true;
}

bool lineAlign(LuaCtx *ctx)
{
  if (ctx->size == 0) return true;

  off_t min = std::min(ctx->size, (off_t) MAX_LINE_LEN);
  lseek(ctx->fd, ctx->size - min, SEEK_SET);

  if (read(ctx->fd, ctx->buffer, MAX_LINE_LEN) != min) {
    return false;
  }

  char *pos = (char *) memrchr(ctx->buffer, NL, min);
  if (!pos) return false;

  ctx->npos = ctx->buffer + min - (pos+1);
  memmove(ctx->buffer, pos+1, ctx->npos);
  return true;
}

/* watch IN_DELETE_SELF | IN_MOVE_SELF does not work
 * luactx hold fd to the deleted file, the file will never be real deleted
 * so DELETE will be inotified
 */
static const uint32_t WATCH_EVENT = IN_MODIFY;

bool addWatch(CnfCtx *ctx, char *errbuf)
{
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin(); ite != ctx->luaCtxs.end(); ++ite) {
    LuaCtx *lctx = *ite;
    
    lctx->fd = open(lctx->file.c_str(), O_RDONLY);
    if (lctx->fd == -1) {
      snprintf(errbuf, MAX_ERR_LEN, "%s open error", lctx->file.c_str());
      return false;
    }
    
    struct stat st;
    fstat(lctx->fd, &st);
    lctx->size = st.st_size;
    lctx->inode = st.st_ino;

    if (!lineAlign(lctx)) {
      snprintf(errbuf, MAX_ERR_LEN, "%s align new line error", lctx->file.c_str());
      return false;
    }

    int wd = inotify_add_watch(ctx->wfd, lctx->file.c_str(), WATCH_EVENT);
    if (wd == -1) {
      snprintf(errbuf, MAX_ERR_LEN, "%s add watch error", lctx->file.c_str());
      return false;
    }
    ctx->wch.insert(std::make_pair(wd, lctx));
  }
  return true;
}

void tryReWatch(CnfCtx *ctx)
{
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin(); ite != ctx->luaCtxs.end(); ++ite) {
    LuaCtx *lctx = *ite;
    if (lctx->fd != -1) continue;
    lctx->fd = open(lctx->file.c_str(), O_RDONLY);
    if (lctx->fd != -1) {
      struct stat st;
      fstat(lctx->fd, &st);

      fprintf(stderr, "may rewatch %d %d\n", st.st_size, lctx->size);

      // if file unlinked or truncated
      if (st.st_ino != lctx->inode || st.st_size < lctx->size) {
        fprintf(stderr, "rewatch %s\n", lctx->file.c_str());
        
        int wd = inotify_add_watch(ctx->wfd, lctx->file.c_str(), WATCH_EVENT);
        ctx->wch.insert(std::make_pair(wd, lctx));
        lctx->inode = st.st_ino;

        lctx->sn = ctx->sn;
        tail2kafka(lctx);
      } else {
        close(lctx->fd);
        lctx->fd = -1;
      }
    }
  }
}

void tryRmWatch(CnfCtx *ctx)
{
  for (WatchCtxHash::iterator ite = ctx->wch.begin(); ite != ctx->wch.end(); ) {
    LuaCtx *lctx = ite->second;
    
    struct stat st;
    fstat(lctx->fd, &st);
    if (st.st_nlink == 0) {
      printf("remove %s\n", ctx->file.c_str());
      inotify_rm_watch(ctx->main->wfd, ite->first);
      ctx->main->wch.erase(ite++);
      close(lctx->fd);
      lctx->fd = -1;
    } else {
      ++ite;
    }
  }
}

inline LuaCtx *wd2ctx(CnfCtx *ctx, int wd)
{
  WatchCtxHash::iterator pos = ctx->wch.find(wd);
  assert(pos != ctx->wch.end());
  return pos->second;
}

bool watchInit(CnfCtx *ctx, char *errbuf)
{
  ctx->wfd = inotify_init1(IN_NONBLOCK);
  if (ctx->wfd == -1) {
    snprintf(errbuf, MAX_ERR_LEN, "inotify_init error");
    return false;
  }

  return addWatch(ctx, errbuf);
}

bool processLine(LuaCtx *ctx, char *line, size_t nline);

bool tail2kafka(LuaCtx *ctx)
{
  struct stat st;
  if (fstat(ctx->fd, &st) != 0) {
    fprintf(stderr, "%s stat error\n", ctx->file.c_str());
    return false;
  }
  ctx->size = st.st_size;

  off_t off = lseek(ctx->fd, 0, SEEK_CUR);
  if (off == (off_t) -1) {
    fprintf(stderr, "%s seek cur error\n", ctx->file.c_str());
    return false;
  }
  
  while (off < ctx->size) {
    size_t min = std::min(ctx->size - off, (off_t) (MAX_LINE_LEN - ctx->npos));
    ssize_t nn = read(ctx->fd, ctx->buffer + ctx->npos, min);
    if (nn == -1) {
      fprintf(stderr, "%s read error\n", ctx->file.c_str());
      return false;
    }

    off += nn;
    ctx->npos += nn;

    size_t n = 0;
    char *pos;
    while ((pos = (char *) memchr(ctx->buffer + n, NL, ctx->npos - n))) {
      processLine(ctx, ctx->buffer + n, (pos+1) - (ctx->buffer + n));
      n = (pos+1) - ctx->buffer;
      if (n == ctx->npos) break;
    }

    if (ctx->npos > n) {
      ctx->npos -= n;
      memmove(ctx->buffer, ctx->buffer + n, ctx->npos);
    } else {
      ctx->npos = 0;
    }
  }
  return true;
}

bool watchLoop(CnfCtx *ctx)
{
  const size_t eventBufferSize = ctx->luaCtxs.size() * ONE_EVENT_SIZE * 2;
  char *eventBuffer = (char *) malloc(eventBufferSize);

  struct pollfd fds[] = {
    {ctx->wfd, POLLIN, 0 }
  };

  while (want != STOP) {
    int nfd = poll(fds, 1, 500);
    if (nfd == -1 && errno != EINTR) break;
    else if (nfd == 0) flushCache(ctx, true);
    else {
      ctx->sn++;    

      ssize_t nn = read(ctx->wfd, eventBuffer, eventBufferSize);
      assert(nn > 0);

      char *p = eventBuffer;
      while (p < eventBuffer + nn) {
        struct inotify_event *event = (struct inotify_event *) p;
        if (event->mask == IN_IGNORED) continue;
        printf("## %d %d##\n", event->mask, IN_IGNORED);
        LuaCtx *lctx = wd2ctx(ctx, event->wd);
        lctx->sn = ctx->sn;
        tail2kafka(lctx);
        p += sizeof(struct inotify_event) + event->len;
      }
      flushCache(ctx, false);
    }
    tryRmWatch(ctx);
    tryReWatch(ctx);
  }
  return true;
}

pid_t spawn(CnfCtx *ctx, char *errbuf)
{
  if (!watchInit(ctx, errbuf)) {
    fprintf(stderr, "watch init error %s\n", errbuf);
    return -1;
  }

  int pid = fork();
  if (pid > 0) {
    bool rc = watchLoop(ctx);
    exit(rc ? EXIT_SUCCESS : EXIT_FAILURE);
  }
  return pid;
}

void split(const char *line, size_t nline, StringList *items)
{
  bool esc = false;
  char want = '\0';
  size_t pos = 0;

  for (size_t i = 0; i < nline; ++i) {
    if (esc) {
      esc = false;
    } else if (line[i] == '\\') {
      esc = true;
    } else if (want == '"') {
      if (line[i] == '"') {
        want = '\0';
        items->push_back(std::string(line + pos, i-pos));
        pos = i+1;
      }
    } else if (want == ']') {
      if (line[i] == ']') {
        want = '\0';
        items->push_back(std::string(line + pos, i-pos));
        pos = i+1;
      }
    } else {
      if (line[i] == '"') {
        want = line[i];
        pos++;
      } else if (line[i] == '[') {
        want = ']';
        pos++;
      } else if (line[i] == ' ') {
        if (i != pos) items->push_back(std::string(line + pos, i - pos));
        pos = i+1;
      }
    }
  }
  if (pos != nline) items->push_back(std::string(line + pos));
}

static const char *MonthAlpha[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// 28/Feb/2015:12:30:23 -> 2015-03-30T16:31:53
bool iso8601(const std::string &t, std::string *iso)
{
  enum { WaitYear, WaitMonth, WaitDay, WaitHour, WaitMin, WaitSec } status = WaitDay;
  int year, mon, day, hour, min, sec;
  year = mon = day = hour = min = sec = 0;
  
  const char *p = t.c_str();
  while (*p) {
    if (*p == '/') {
      if (status == WaitDay) status = WaitMonth;
      else if (status == WaitMonth) status = WaitYear;
      else return false;
    } else if (*p == ':') {
      if (status == WaitYear) status = WaitHour;
      else if (status == WaitHour) status = WaitMin;
      else if (status == WaitMin) status = WaitSec;
      else return false;
    } else if (*p >= '0' && *p <= '9') {
      int n = *p - '0';
      if (status == WaitYear) year = year * 10 + n;
      else if (status == WaitDay) day = day * 10 + n;
      else if (status == WaitHour) hour = hour * 10 + n;
      else if (status == WaitMin) min = min * 10 + n;
      else if (status == WaitSec) sec = sec * 10 + n;
      else return false;
    } else if (status == WaitMonth) {
      size_t i;
      for (i = 0; i < 12; ++i) {
        if (strcmp(p, MonthAlpha[i]) == 0) {
          mon = i;
          break;
        }
      }
    } else {
      return false;
    }
  }

  iso->resize(sizeof("yyyy-mm-ddThh:mm:ss"));
  sprintf((char *) iso->data(), "%04d-%02d-%02dT%02d:%02d:%02d",
          year, mon, day, hour, min, sec);
  return true;
}

bool processLine(LuaCtx *ctx, char *line, size_t nline)
{
  std::string *data = 0;
  StringPtrList *datas = new StringPtrList;
  bool rc;
  
  if (ctx->transform) {
    data = new std::string;
    rc = ctx->transform(ctx, line, nline-1, data);
  } else if (ctx->aggregate || ctx->filter) {
    StringList fields;
    split(line, nline-1, &fields);
    
    if (ctx->timeidx != UNSET_INT) {
      int idx = absidx(ctx->timeidx, fields.size());
      if (idx < 0 || (size_t) idx >= fields.size()) return false;
      iso8601(fields[idx], &fields[idx]);
    }

    if (ctx->aggregate) {
      rc = ctx->aggregate(ctx, fields, datas);
    } else {
      data = new std::string;
      rc = ctx->filter(ctx, fields, data);
    }
  } else {
    data = new std::string(line, nline);
  }

  if (data) {
    if (!data->empty()) datas->push_back(data);
    else delete data;
  }
  
  if (datas->empty()) {
    delete datas;
  } else {
    OneTaskReq req = {ctx->idx, datas};
    ssize_t nn = write(ctx->main->server, &req, sizeof(OneTaskReq));
    assert(nn != -1 && nn == sizeof(OneTaskReq));
  }

  return rc;
}

void dr_cb(rd_kafka_t *, void *, size_t, rd_kafka_resp_err_t, void *, void *data)
{
  std::string *p = (std::string *) data;
  delete p;
}

bool initKafka(CnfCtx *ctx)
{
  char errstr[512];
  
  ctx->conf = rd_kafka_conf_new();
  for (SSHash::iterator ite = ctx->kafkaGlobal.begin();
       ite != ctx->kafkaGlobal.end(); ++ite) {
    rd_kafka_conf_res_t res;
    res = rd_kafka_conf_set(ctx->conf, ite->first.c_str(), ite->second.c_str(),
                            errstr, sizeof(errstr));
    if (res != RD_KAFKA_CONF_OK) {
      fprintf(stderr, "kafka conf %s=%s %s",
              ite->first.c_str(), ite->second.c_str(), errstr);
      return false;
    }
  }
  rd_kafka_conf_set_dr_cb(ctx->conf, dr_cb);

  ctx->tconf = rd_kafka_topic_conf_new();
  for (SSHash::iterator ite = ctx->kafkaTopic.begin();
       ite != ctx->kafkaTopic.end(); ++ite) {
    rd_kafka_conf_res_t res;    
    res = rd_kafka_topic_conf_set(ctx->tconf, ite->first.c_str(), ite->second.c_str(),
                                  errstr, sizeof(errstr));
    if (res != RD_KAFKA_CONF_OK) {
      fprintf(stderr, "kafka topic conf %s=%s %s\n",
              ite->first.c_str(), ite->second.c_str(), errstr);
      return false;
    }
  }

  ctx->rk = rd_kafka_new(RD_KAFKA_PRODUCER, ctx->conf, errstr, sizeof(errstr));
  if (!ctx->rk) {
    fprintf(stderr, "new kafka produce error %s\n", errstr);
    return false;
  }

  if (rd_kafka_brokers_add(ctx->rk, ctx->brokers.c_str()) < 1) {
    fprintf(stderr, "kafka invalid brokers %s\n", ctx->brokers.c_str());
    return false;
  }

  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin(); ite != ctx->luaCtxs.end(); ++ite) {
    rd_kafka_topic_t *rkt;
    rkt = rd_kafka_topic_new(ctx->rk, (*ite)->topic.c_str(), ctx->tconf);
    if (!rkt) return false;
    ctx->rkts.push_back(rkt);
  }

  return true;
}

void uninitKafka(CnfCtx *ctx)
{
  for (TopicList::iterator ite = ctx->rkts.begin(); ite != ctx->rkts.end(); ++ite) {
    rd_kafka_topic_destroy(*ite);
  }
  if (ctx->rk) rd_kafka_destroy(ctx->rk);
  if (ctx->tconf) rd_kafka_topic_conf_destroy(ctx->tconf);
  if (ctx->conf) rd_kafka_conf_destroy(ctx->conf);
}
    
void *routine(void *data)
{
  CnfCtx *ctx = (CnfCtx *) data;
  OneTaskReq req;

  while (true) {
    ssize_t nn = read(ctx->accept, &req, sizeof(OneTaskReq));
    if (nn == -1 && errno != EINTR) {
      break;
    }
    if (nn != sizeof(OneTaskReq)) {
      fprintf(stderr, "invalid req size %d\n", (int) nn);
      break;
    }

    rd_kafka_topic_t *rkt = ctx->rkts[req.idx];

    int eno;
    if (req.datas->size() == 1) {
      eno = rd_kafka_produce(rkt, RD_KAFKA_PARTITION_UA, 0,
                             (void *) req.datas->at(0)->c_str(), req.datas->at(0)->size(),
                             0, 0, req.datas->at(0));
    } else {
      size_t n = req.datas->size();
      rd_kafka_message_t *rkmsgs = new rd_kafka_message_t[n];
      for (size_t i = 0; i < n; ++i) {
        rkmsgs[i].payload = (void *) req.datas->at(i)->c_str();
        rkmsgs[i].len     = req.datas->at(i)->size();
        rkmsgs[i].key     = 0;
        rkmsgs[i].key_len = 0;
        rkmsgs[i]._private = req.datas->at(i);
      }
      rd_kafka_produce_batch(rkt, RD_KAFKA_PARTITION_UA, 0,
                             rkmsgs, n);
      delete[] rkmsgs;
    }
    delete req.datas;
    
    rd_kafka_poll(ctx->rk, 0);
  }

  return NULL;
}

#ifdef UNITTEST
#define check(r, fmt, arg...) \
  do { if (!(r)) { fprintf(stderr, "%04d %s -> "fmt"\n", __LINE__, #r, ##arg); abort(); } } while(0)

#define TEST(x) test_##x

void TEST(split)()
{
  StringList list;
  
  const char *s1 = "hello \"1 [] 2\"[world] [] [\"\"]  bj";
  split(s1, strlen(s1), &list);
  check(list.size() == 6, "%d", (int) list.size());
  assert(list[0] == "hello");
  assert(list[1] == "1 [] 2");
  assert(list[2] == "world");
  assert(list[3] == "");
  check(list[4] == "\"\"", "%s", list[4].c_str());
  assert(list[5] == "bj");
}

void TEST(loadLuaCtx)()
{
  char errbuf[MAX_ERR_LEN];
  LuaCtx *ctx;
  
  ctx = loadLuaCtx("./basic.lua", errbuf);
  check(ctx, "%s", errbuf);
  check(ctx->fd == -1, "%d", ctx->fd);
  check(ctx->file == "./basic.log", "%s", ctx->file.c_str());
  check(ctx->topic == "basic", "%s", ctx->topic.c_str());
  unloadLuaCtx(ctx);

  ctx = loadLuaCtx("./filter.lua", errbuf);
  check(ctx, "%s", errbuf);
  check(ctx->autosplit == false, "%s", (ctx->autosplit ? "TRUE" : "FALSE"));
  check(ctx->timeidx == 4, "%d", ctx->timeidx);
  check(ctx->filters.size() == 4, "%d", (int) ctx->filters.size());
  check(ctx->filters[0] == 4, "%d", ctx->filters[0]);
  check(ctx->filters[1] == 5, "%d", ctx->filters[1]);
  check(ctx->filters[2] == 6, "%d", ctx->filters[2]);
  check(ctx->filters[3] == -1, "%d", ctx->filters[3]);
  check(ctx->filter, "%s", (ctx->filter ? "FUNC" : "NULL"));
  unloadLuaCtx(ctx);

  ctx = loadLuaCtx("./aggregate.lua", errbuf);
  check(ctx, "%s", errbuf);
  check(ctx->autosplit == true, "%s", (ctx->autosplit ? "TRUE" : "FALSE"));
  check(ctx->withhost == true, "%s", (ctx->withhost ? "TRUE" : "FALSE"));
  check(ctx->withtime == true, "%s", (ctx->withtime ? "TRUE" : "FALSE"));
  check(ctx->aggregate, "%s", (ctx->aggregate ? "FUNC" : "NULL"));
  unloadLuaCtx(ctx);

  ctx = loadLuaCtx("./transform.lua", errbuf);
  check(ctx, "%s", errbuf);
  check(ctx->transform, "%s", (ctx->transform ? "FUNC" : "NULL"));  
}

void TEST(loadCnf)()
{
  char errbuf[MAX_ERR_LEN];
  CnfCtx *ctx;
  
  ctx = loadCnf(".", errbuf);
  check(ctx != 0, "loadCnf . %s", errbuf);
  
  assert(ctx->conf == 0);
  assert(ctx->tconf == 0);
  assert(ctx->rk == 0);

  check(ctx->host == "zzyong.paas.user.vm", "%s", ctx->host.c_str());
  check(ctx->brokers == "127.0.0.1:9092", "%s", ctx->brokers.c_str());
  check(ctx->kafkaGlobal["client.id"] == "tail2kafka",
        "%s", ctx->kafkaGlobal["client.id"].c_str());
  check(ctx->kafkaTopic["request.required.acks"] == "1",
        "%s", ctx->kafkaTopic["request.required.acks"].c_str());
  
  check(ctx->luaCtxs.size() == 5, "%d", (int) ctx->luaCtxs.size());
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin(); ite != ctx->luaCtxs.end(); ++ite) {
    assert((*ite)->main == ctx);
  }
  unloadCnfCtx(ctx);
}

void TEST(transform)()
{
  char errbuf[MAX_ERR_LEN];
  CnfCtx *main;
  LuaCtx *ctx;
  std::string data;

  main = loadCnf(".", errbuf);
  assert(main != 0);

  ctx = loadLuaCtx("./transform.lua", errbuf);
  assert(ctx != 0);
  ctx->main = main;

  ctx->transform(ctx, "[error] this", sizeof("[error] this")-1, &data);
  check(data == main->host + " [error] this", "'%s'", data.c_str());

  ctx->withhost = false;
  ctx->transform(ctx, "[error] this", sizeof("[error] this")-1, &data);
  check(data == "[error] this", "'%s'", data.c_str());  

  ctx->transform(ctx, "[debug] that", sizeof("[debug] that")-1, &data);
  check(data.empty() == true, "'%s'", data.empty() ? "TRUE" : "FALSE");

  unloadLuaCtx(ctx);  
}

void TEST(aggregate)()
{
  char errbuf[MAX_ERR_LEN];
  CnfCtx *main;
  LuaCtx *ctx;
  StringPtrList datas;
    
  main = loadCnf(".", errbuf);
  assert(main != 0);

  ctx = loadLuaCtx("./aggregate.lua", errbuf);
  assert(ctx != 0);
  ctx->main = main;

  const char *fields1[] = {
    "-", "-", "-", "2015-04-02T12:05:04", "-",
    "-", "-", "-", "200", "230",
    "0.1", "-", "-", "-", "-",
    "10086"};

  ctx->aggregate(ctx, StringList(fields1, fields1 + 16), &datas);
  check(datas.empty() == true, "%s", datas.empty() ? "TRUE" : "FALSE");

  const char *fields2[] = {
    "-", "-", "-", "2015-04-02T12:05:04", "-",
    "-", "-", "-", "200", "270",
    "0.2", "-", "-", "-", "-",
    "10086"};
  
  ctx->aggregate(ctx, StringList(fields2, fields2 + 16), &datas);
  check(datas.empty() == true, "%s", datas.empty() ? "TRUE" : "FALSE");  

  const char *fields3[] = {
    "-", "-", "-", "2015-04-02T12:05:05", "-",
    "-", "-", "-", "404", "250",
    "0.2", "-", "-", "-", "-",
    "95555"};
  ctx->aggregate(ctx, StringList(fields3, fields3 + 16), &datas);
  check(datas.size() == 1, "%d", (int) datas.size());
  const char *msg = "2015-04-02T12:05:04 10086 reqt<0.1=1 reqt<0.3=1 size=500 status_200=2";
  check((*datas[0]) == main->host + " " + msg, "%s", datas[0]->c_str());
  
  unloadLuaCtx(ctx);  
}

void TEST(grep)()
{
  char errbuf[MAX_ERR_LEN];
  CnfCtx *main;
  LuaCtx *ctx;
  std::string data;
    
  main = loadCnf(".", errbuf);
  assert(main != 0);

  ctx = loadLuaCtx("./grep.lua", errbuf);
  assert(ctx != 0);
  ctx->main = main;
  
  const char *fields1[] = {
    "-", "-", "-", "2015-04-02T12:05:05", "GET / HTTP/1.0",
    "200", "-", "-", "95555"};
  
  ctx->grep(ctx, StringList(fields1, fields1+9), &data);
  check(data == main->host + " 2015-04-02T12:05:05 \"GET / HTTP/1.0\" 200 95555",
        "%s", data.c_str());

  unloadLuaCtx(ctx);
}

void TEST(filter)()
{
  char errbuf[MAX_ERR_LEN];
  CnfCtx *main;
  LuaCtx *ctx;
  std::string data;
    
  main = loadCnf(".", errbuf);
  assert(main != 0);

  ctx = loadLuaCtx("./filter.lua", errbuf);
  assert(ctx != 0);
  ctx->main = main;
  
  const char *fields1[] = {
    "-", "-", "-", "2015-04-02T12:05:05", "GET / HTTP/1.0",
    "200", "-", "-", "95555"};
  
  ctx->filter(ctx, StringList(fields1, fields1+9), &data);
  check(data == main->host + " 2015-04-02T12:05:05 GET / HTTP/1.0 200 95555",
        "%s", data.c_str());

  unloadLuaCtx(ctx);  
}

void *TEST(watchLoop)(void *data)
{
  CnfCtx *ctx = (CnfCtx *) data;
  watchLoop(ctx);
  return 0;
}

void TEST(watchInit)()
{
  bool rc;
  char errbuf[MAX_ERR_LEN];
  CnfCtx *ctx;

  int fd = open("./basic.log", O_WRONLY);
  assert(fd != -1);
  write(fd, "12\n456", 6);

  ctx = loadCnf(".", errbuf);
  rc = watchInit(ctx, errbuf);
  check(rc == true, "%s", errbuf);

  LuaCtx *lctx;
  for (LuaCtxPtrList::iterator ite = ctx->luaCtxs.begin(); ite != ctx->luaCtxs.end(); ++ite) {
    if ((*ite)->topic == "basic") {
      lctx = *ite;
      off_t cur = lseek(lctx->fd, 0, SEEK_CUR);
      check(lctx->size == cur, "%d = %d", (int) lctx->size, (int) cur);
      check(lctx->size == 6, "%d", (int) lctx->size);
      check(lctx->npos == 3, "%d", (int) lctx->npos);
      check(std::string(lctx->buffer, lctx->npos) == "456", "%.*s", (int) lctx->npos, lctx->buffer);
      break;
    }
  }

  pthread_t tid;
  pthread_create(&tid, NULL, TEST(watchLoop), ctx);

  write(fd, "\n789\n", 5);
  close(fd);

  sleep(100);
  unlink("./basic.log");

  OneTaskReq req;
  read(ctx->accept, &req, sizeof(OneTaskReq));

  /* test wath */
  check(req.idx == lctx->idx, "%d = %d", req.idx, lctx->idx);
  check(*(req.datas->at(0)) == "456\n", "%s", req.datas->at(0)->c_str());

  read(ctx->accept, &req, sizeof(OneTaskReq));
  check(*(req.datas->at(0)) == "789\n", "%s", req.datas->at(0)->c_str());

  check(lctx->size == 11, "%d", (int) lctx->size);

  /* check rmwatch */
  sleep(1);
  for (WatchCtxHash::iterator ite = ctx->wch.begin(); ite != ctx->wch.end(); ++ite) {
    check(ite->second != lctx, "%s should be remove from inotify", lctx->file.c_str());
  }
  
  /* test rewatch */
  fd = open("./basic.log", O_CREAT | O_WRONLY, 0644);
  assert(fd != -1);
  write(fd, "abcd\n", 5);
  close(fd);

  read(ctx->accept, &req, sizeof(OneTaskReq));
  check(*(req.datas->at(0)) == "abcd\n", "%s", req.datas->at(0)->c_str());

  want = STOP;
  pthread_join(tid, 0);
}

static const char *files[] = {
  "./basic.log",
  "./access_log",
  "./nginx.log",
  "./error.log",
  0
};

void TEST(prepare)()
{
  for (int i = 0; files[i]; ++i) {
    int fd = creat(files[i], 0644);
    if (fd != -1) close(fd);
  }
}

void TEST(clean)()
{
  for (int i = 0; files[i]; ++i) {
    unlink(files[i]);
  }
}
  
int main(int argc, char *argv[])
{
  TEST(prepare)();
    
  TEST(split)();
  TEST(loadLuaCtx)();
  TEST(loadCnf)();
  
  TEST(transform)();
  TEST(aggregate)();
  TEST(grep)();
  TEST(filter)();

  TEST(watchInit)();

  TEST(clean)();
  printf("OK\n");
  return 0;
}
#endif
