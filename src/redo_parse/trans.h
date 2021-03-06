#ifndef TRANS_INC
#define TRANS_INC
#include <map>
#include <set>
#include <vector>
#include <memory>
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>
#include <algorithm>
#include "easylogging++.h"
#include "logical_elems.h"
#include "opcode.h"
#include "util/dtypes.h"
#include "logical_elems.h"

namespace databus {

  struct ColumnLess {
    bool operator()(const std::shared_ptr<ColumnChange>& l,
                    const std::shared_ptr<ColumnChange>& r) {
      return l->col_id_ < r->col_id_;
    }
  };

  const std::set<char> cflag{0x00, 0x10, 0x02, 0x12, 0x04, 0x14};

  struct RowChange;
  class TabDef;
  typedef std::shared_ptr<RowChange> RowChangePtr;
  typedef std::set<ColumnChangePtr, ColumnLess> OrderedPK;

  struct RowChange {
    RowChange();
    RowChange(SCN& scn, uint32_t obj_id, Ushort op, Ushort uflag, Ushort iflag,
              Row& undo, Row& redo);
    bool operator<(const RowChange& other) const { return scn_ < other.scn_; }
    std::string toString() const;
    std::string pkToString() const;
    std::vector<std::string> getPk();
    bool completed() const;

    SCN scn_;
    uint32_t object_id_;
    Ushort op_;
    Ushort start_col_;
    uint32_t epoch_;
    // we only care about cc for 11.6
    // see https://jirap.corp.ebay.com/browse/DBISTREA-37
    Ushort cc_;
    Ushort uflag_;
    Uchar iflag_;
    OrderedPK old_pk_;
    OrderedPK new_pk_;
  };

  struct Less {
    bool operator()(const std::shared_ptr<RowChange>& l,
                    const std::shared_ptr<RowChange>& r) {
      return l->scn_ < r->scn_;
    }
  };

  struct Transaction;
  typedef std::map<XID, std::shared_ptr<Transaction>> XIDMap;
  typedef std::map<DBA, USN> DBAMap;

  struct TimePoint {
    TimePoint() : epoch_(0), scn_() {}
    TimePoint(const SCN& scn, const uint32_t epoch)
        : epoch_(epoch), scn_(scn) {}
    uint32_t epoch_;
    SCN scn_;
    std::string toString() const;
    bool empty() const { return epoch_ == 0 and scn_.empty(); }
  };

  struct Transaction {
   public:
    Transaction()
        : cflag_(-1),
          xid_(0),
          start_scn_(),
          commit_scn_(),
          end_epoch_(0),
          start_epoch_(0) {}
    XID xid_;
    SCN start_scn_;
    SCN commit_scn_;
    char cflag_;
    uint32_t start_epoch_;
    uint32_t end_epoch_;

    // orginize changes, for row-chains, row migration
    std::set<RowChangePtr, Less> changes_;
    std::string toString() const;

    bool operator<(const Transaction& other) const;
    bool has_rollback() const { return cflag_ & 4; }
    bool has_commited() const { return !(still_pending() || has_rollback()); }
    bool still_pending() const { return cflag_ == -1; }
    bool empty() const { return changes_.empty(); }
    void tidyChanges();

    static XIDMap xid_map_;
    static std::map<SCN, std::shared_ptr<Transaction>> commit_trans_;

   private:
    void merge(RowChangePtr r);
    bool lastCompleted() const;

   public:
    static DBAMap dba_map_;
    static std::map<SCN, uint32_t> start_scn_q_;

   public:
    static TimePoint getLastCommitTimePoint() {
      std::lock_guard<std::mutex> lk(commit_mutex_);
      return TimePoint(last_commit_scn_, last_commit_epoch_);
    }

    static TimePoint getRestartTimePoint() {
      std::lock_guard<std::mutex> lk(commit_mutex_);
      return TimePoint(restart_scn_, restart_epoch_);
    }

    static void setRestartTimePoint(const SCN& scn, uint32_t epoch) {
      std::lock_guard<std::mutex> lk(restart_mutex_);
      restart_scn_ = scn;
      restart_epoch_ = epoch;
    }

    static void setLastCommitTimePoint(const SCN& scn, uint32_t epoch) {
      std::lock_guard<std::mutex> lk(commit_mutex_);
      last_commit_scn_ = scn;
      last_commit_epoch_ = epoch;
    }

    static void eraseStartScn(const SCN& scn) {
      start_scn_q_.erase(scn);
      if (scn == restart_scn_) {
        if (!start_scn_q_.empty()) {
          auto it = start_scn_q_.begin();
          setRestartTimePoint(it->first, it->second);
        }
      }
    }

    static void setTimePointWhenCommit(
        const std::shared_ptr<Transaction> tran) {
      if (last_commit_scn_ < tran->commit_scn_) {
        setLastCommitTimePoint(tran->commit_scn_, tran->end_epoch_);
      }
      eraseStartScn(tran->start_scn_);
    }

    static uint32_t removeUncompletedTrans() {
      uint32_t n = 0;
      auto it = xid_map_.begin();
      while (it != xid_map_.end()) {
        if (it->second->start_scn_.empty()) {
          it = xid_map_.erase(it);
          n += 1;
        } else {
          ++it;
        }
      }
      return n;
    }

   private:
    static std::mutex restart_mutex_;
    static std::mutex commit_mutex_;
    static uint32_t restart_epoch_;
    static uint32_t last_commit_epoch_;
    static SCN last_commit_scn_;
    static SCN restart_scn_;
  };

  XIDMap::iterator buildTransaction(XIDMap::iterator it);

  typedef std::shared_ptr<Transaction> TransactionPtr;
  typedef std::map<SCN, TransactionPtr>& SCNTranMap;

  void addToTransaction(RecordBufPtr ptr);
  void makeTranRecord(XID xid, RowChangePtr rcp, std::list<Row>& undo,
                      std::list<Row>& redo);

  bool verifyTrans(TransactionPtr trans_ptr);
  Ushort findPk(std::shared_ptr<TabDef> tab_def, const Row& undo,
                OrderedPK& pk);
  std::string colAsStr2(ColumnChangePtr col, std::shared_ptr<TabDef> tab_def);
}
#endif /* ----- #ifndef TRANS_INC  ----- */
