#ifndef _CNFCTX_H_
#define _CNFCTX_H_

#include <string>
#include <vector>
#include <map>
#include <sys/time.h>

#include "gnuatomic.h"
#include "fileoff.h"
#include "luahelper.h"
#include "esctx.h"
#include "kafkactx.h"
#include "common.h"

#define KAFKA_ERROR_TIMEOUT 60

class RunStatus;

enum TimeUnit {
  TIMEUNIT_MILLI, TIMEUNIT_SECONDS,
};

class CnfCtx {
  template<class T> friend class UNITTEST_HELPER;
public:
  int                    accept;
  int                    server;

public:
  static CnfCtx *loadCnf(const char *dir, char *errbuf);
  bool reset();
  bool rectifyHistoryFile();

  static CnfCtx *loadFile(const char *file, char *errbuf);
  ~CnfCtx();
  void addLuaCtx(LuaCtx *ctx);

  bool enableKafka() const { return !brokers_.empty(); }
  bool initKafka();
  KafkaCtx *getKafka() { return kafka_; }

  bool enableEs() const { return !esNodes_.empty(); }
  bool initEs();
  EsCtx *getEs() { return es_; }

  bool initFileOff();
  FileOff *getFileOff() { return fileOff_; }

  bool initFileReader();

  void setRunStatus(RunStatus *runStatus) { runStatus_ = runStatus; }
  RunStatus *getRunStatus() { return runStatus_; }

  const char *getBrokers() const { return brokers_.c_str(); }
  const std::map<std::string, std::string> &getKafkaGlobalConf() const { return kafkaGlobal_; }
  const std::map<std::string, std::string> &getKafkaTopicConf() const { return kafkaTopic_; }

  const char *getEsNodes() const { return esNodes_.c_str(); }
  size_t getEsMaxConns() const { return esMaxConns_; }

  const char *getPidFile() const {
    return pidfile_.c_str();
  }

  LuaHelper *getLuaHelper() { return helper_; }

  size_t getLuaCtxSize() const { return count_; }
  std::vector<LuaCtx *> &getLuaCtxs() { return luaCtxs_; }

  int getPollLimit() const { return pollLimit_; }
  int getRotateDelay() const { return rotateDelay_; }
  const std::string &pingbackUrl() const { return pingbackUrl_; }

  uint32_t addr() const { return addr_; }
  int partition() const { return partition_; }
  const std::string &host() const { return host_; }

  int64_t fasttime(TimeUnit unit = TIMEUNIT_SECONDS) const {
    if (unit == TIMEUNIT_MILLI) return timeval_.tv_sec * 1000 + timeval_.tv_usec / 1000;
    else return timeval_.tv_sec;
  }

  int64_t fasttime(bool force, TimeUnit unit) {
    if (force) gettimeofday(&timeval_, 0);
    return fasttime(unit);
  }

  char *errbuf() { return errbuf_; }
  const std::string &libdir() const { return libdir_; }
  const std::string &logdir() const { return logdir_; }

  void setTailLimit(bool tailLimit) { tailLimit_ = tailLimit; }
  bool getTailLimit() const { return tailLimit_; }

  void setKafkaBlock(bool kafkaBlock) { util::atomic_set(&kafkaBlock_, kafkaBlock ? 1 : 0); }
  bool getKafkaBlock() const { return util::atomic_get((int *) &kafkaBlock_); }

private:
  CnfCtx();

  std::string pidfile_;
  std::string host_;
  uint32_t    addr_;
  int         partition_;
  int         pollLimit_;
  int         rotateDelay_;
  std::string pingbackUrl_;
  std::string logdir_;
  std::string libdir_;

  size_t                 count_;
  std::vector<LuaCtx *>  luaCtxs_;

  std::string                         brokers_;
  std::map<std::string, std::string>  kafkaGlobal_;
  std::map<std::string, std::string>  kafkaTopic_;
  KafkaCtx                           *kafka_;

  std::string  esNodes_;
  int          esMaxConns_;
  EsCtx       *es_;

  struct timeval timeval_;
  char        *errbuf_;
  RunStatus   *runStatus_;

  LuaHelper  *helper_;
  FileOff    *fileOff_;

  bool tailLimit_;
  int kafkaBlock_;
};

#endif
