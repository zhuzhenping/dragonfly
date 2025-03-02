// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/zset_family.h"

extern "C" {
#include "redis/listpack.h"
#include "redis/object.h"
#include "redis/util.h"
#include "redis/zset.h"
}

#include <absl/strings/charconv.h>

#include "base/logging.h"
#include "base/stl_util.h"
#include "facade/error.h"
#include "server/command_registry.h"
#include "server/conn_context.h"
#include "server/engine_shard_set.h"
#include "server/transaction.h"

namespace dfly {

using namespace std;
using namespace facade;
using absl::SimpleAtoi;
namespace {

using CI = CommandId;

static const char kNxXxErr[] = "XX and NX options at the same time are not compatible";
static const char kScoreNaN[] = "resulting score is not a number (NaN)";
static const char kFloatRangeErr[] = "min or max is not a float";
static const char kLexRangeErr[] = "min or max not valid string range item";

constexpr unsigned kMaxListPackValue = 64;

inline zrangespec GetZrangeSpec(bool reverse, const ZSetFamily::ScoreInterval& si) {
  auto interval = si;
  if (reverse)
    swap(interval.first, interval.second);

  zrangespec range;
  range.min = interval.first.val;
  range.max = interval.second.val;
  range.minex = interval.first.is_open;
  range.maxex = interval.second.is_open;

  return range;
}

sds GetLexStr(const ZSetFamily::LexBound& bound) {
  if (bound.type == ZSetFamily::LexBound::MINUS_INF)
    return cminstring;

  if (bound.type == ZSetFamily::LexBound::PLUS_INF)
    return cmaxstring;

  return sdsnewlen(bound.val.data(), bound.val.size());
};

zlexrangespec GetLexRange(bool reverse, const ZSetFamily::LexInterval& li) {
  auto interval = li;
  if (reverse)
    swap(interval.first, interval.second);

  zlexrangespec range;
  range.minex = 0;
  range.maxex = 0;

  range.min = GetLexStr(interval.first);
  range.max = GetLexStr(interval.second);
  range.minex = (interval.first.type == ZSetFamily::LexBound::OPEN);
  range.maxex = (li.second.type == ZSetFamily::LexBound::OPEN);

  return range;
}

struct ZParams {
  unsigned flags = 0;  // mask of ZADD_IN_ macros.
  bool ch = false;     // Corresponds to CH option.
  bool override = false;
};

OpResult<PrimeIterator> FindZEntry(const ZParams& zparams, const OpArgs& op_args, string_view key,
                                   size_t member_len) {
  auto& db_slice = op_args.shard->db_slice();
  if (zparams.flags & ZADD_IN_XX) {
    return db_slice.Find(op_args.db_ind, key, OBJ_ZSET);
  }

  pair<PrimeIterator, bool> add_res;

  try {
    add_res = db_slice.AddOrFind(op_args.db_ind, key);
  } catch (bad_alloc&) {
    return OpStatus::OUT_OF_MEMORY;
  }

  PrimeIterator& it = add_res.first;
  if (add_res.second || zparams.override) {
    robj* zobj = nullptr;

    if (member_len > kMaxListPackValue) {
      zobj = createZsetObject();
    } else {
      zobj = createZsetListpackObject();
    }

    DVLOG(2) << "Created zset " << zobj->ptr;
    if (!add_res.second) {
      db_slice.PreUpdate(op_args.db_ind, it);
    }
    it->second.ImportRObj(zobj);
  } else {
    if (it->second.ObjType() != OBJ_ZSET)
      return OpStatus::WRONG_TYPE;
    db_slice.PreUpdate(op_args.db_ind, it);
  }

  return it;
}

enum class Action {
  RANGE = 0,
  REMOVE = 1,
};

class IntervalVisitor {
 public:
  IntervalVisitor(Action action, const ZSetFamily::RangeParams& params, robj* o)
      : action_(action), params_(params), zobj_(o) {
  }

  void operator()(const ZSetFamily::IndexInterval& ii);

  void operator()(const ZSetFamily::ScoreInterval& si);

  void operator()(const ZSetFamily::LexInterval& li);

  ZSetFamily::ScoredArray PopResult() {
    return std::move(result_);
  }

  unsigned removed() const {
    return removed_;
  }

 private:
  void ExtractListPack(const zrangespec& range);
  void ExtractSkipList(const zrangespec& range);

  void ExtractListPack(const zlexrangespec& range);
  void ExtractSkipList(const zlexrangespec& range);

  void ActionRange(unsigned start, unsigned end);  // rank
  void ActionRange(const zrangespec& range);       // score
  void ActionRange(const zlexrangespec& range);    // lex

  void ActionRem(unsigned start, unsigned end);  // rank
  void ActionRem(const zrangespec& range);       // score
  void ActionRem(const zlexrangespec& range);    // lex

  void Next(uint8_t* zl, uint8_t** eptr, uint8_t** sptr) const {
    if (params_.reverse) {
      zzlPrev(zl, eptr, sptr);
    } else {
      zzlNext(zl, eptr, sptr);
    }
  }

  zskiplistNode* Next(zskiplistNode* ln) const {
    return params_.reverse ? ln->backward : ln->level[0].forward;
  }

  bool IsUnder(double score, const zrangespec& spec) const {
    return params_.reverse ? zslValueGteMin(score, &spec) : zslValueLteMax(score, &spec);
  }

  void AddResult(const uint8_t* vstr, unsigned vlen, long long vlon, double score);

  Action action_;
  ZSetFamily::RangeParams params_;
  robj* zobj_;

  ZSetFamily::ScoredArray result_;
  unsigned removed_ = 0;
};

void IntervalVisitor::operator()(const ZSetFamily::IndexInterval& ii) {
  unsigned long llen = zsetLength(zobj_);
  int32_t start = ii.first;
  int32_t end = ii.second;

  if (start < 0)
    start = llen + start;
  if (end < 0)
    end = llen + end;
  if (start < 0)
    start = 0;

  if (start > end || unsigned(start) >= llen) {
    return;
  }

  if (unsigned(end) >= llen)
    end = llen - 1;

  switch (action_) {
    case Action::RANGE:
      ActionRange(start, end);
      break;
    case Action::REMOVE:
      ActionRem(start, end);
      break;
  }
}

void IntervalVisitor::operator()(const ZSetFamily::ScoreInterval& si) {
  zrangespec range = GetZrangeSpec(params_.reverse, si);

  switch (action_) {
    case Action::RANGE:
      ActionRange(range);
      break;
    case Action::REMOVE:
      ActionRem(range);
      break;
  }
}

void IntervalVisitor::operator()(const ZSetFamily::LexInterval& li) {
  zlexrangespec range = GetLexRange(params_.reverse, li);

  switch (action_) {
    case Action::RANGE:
      ActionRange(range);
      break;
    case Action::REMOVE:
      ActionRem(range);
      break;
  }
  zslFreeLexRange(&range);
}

void IntervalVisitor::ActionRange(unsigned start, unsigned end) {
  unsigned rangelen = (end - start) + 1;

  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj_->ptr;
    uint8_t *eptr, *sptr;
    uint8_t* vstr;
    unsigned int vlen;
    long long vlong;
    double score = 0.0;

    if (params_.reverse)
      eptr = lpSeek(zl, -2 - long(2 * start));
    else
      eptr = lpSeek(zl, 2 * start);
    DCHECK(eptr);

    sptr = lpNext(zl, eptr);

    while (rangelen--) {
      DCHECK(eptr != NULL && sptr != NULL);
      vstr = lpGetValue(eptr, &vlen, &vlong);

      if (params_.with_scores) /* don't bother to extract the score if it's gonna be ignored. */
        score = zzlGetScore(sptr);

      AddResult(vstr, vlen, vlong, score);

      Next(zl, &eptr, &sptr);
    }
  } else {
    CHECK_EQ(zobj_->encoding, OBJ_ENCODING_SKIPLIST);
    zset* zs = (zset*)zobj_->ptr;
    zskiplist* zsl = zs->zsl;
    zskiplistNode* ln;

    /* Check if starting point is trivial, before doing log(N) lookup. */
    if (params_.reverse) {
      ln = zsl->tail;
      unsigned long llen = zsetLength(zobj_);
      if (start > 0)
        ln = zslGetElementByRank(zsl, llen - start);
    } else {
      ln = zsl->header->level[0].forward;
      if (start > 0)
        ln = zslGetElementByRank(zsl, start + 1);
    }

    while (rangelen--) {
      DCHECK(ln != NULL);
      sds ele = ln->ele;
      result_.emplace_back(string(ele, sdslen(ele)), ln->score);
      ln = Next(ln);
    }
  }
}

void IntervalVisitor::ActionRange(const zrangespec& range) {
  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    ExtractListPack(range);
  } else {
    CHECK_EQ(zobj_->encoding, OBJ_ENCODING_SKIPLIST);
    ExtractSkipList(range);
  }
}

void IntervalVisitor::ActionRange(const zlexrangespec& range) {
  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    ExtractListPack(range);
  } else {
    CHECK_EQ(zobj_->encoding, OBJ_ENCODING_SKIPLIST);
    ExtractSkipList(range);
  }
}

void IntervalVisitor::ActionRem(unsigned start, unsigned end) {
  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj_->ptr;

    removed_ = (end - start) + 1;
    zl = lpDeleteRange(zl, 2 * start, 2 * removed_);
    zobj_->ptr = zl;
  } else {
    CHECK_EQ(OBJ_ENCODING_SKIPLIST, zobj_->encoding);
    zset* zs = (zset*)zobj_->ptr;
    removed_ = zslDeleteRangeByRank(zs->zsl, start + 1, end + 1, zs->dict);
  }
}

void IntervalVisitor::ActionRem(const zrangespec& range) {
  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj_->ptr;
    unsigned long deleted = 0;
    zl = zzlDeleteRangeByScore(zl, &range, &deleted);
    zobj_->ptr = zl;
    removed_ = deleted;
  } else {
    CHECK_EQ(OBJ_ENCODING_SKIPLIST, zobj_->encoding);
    zset* zs = (zset*)zobj_->ptr;
    removed_ = zslDeleteRangeByScore(zs->zsl, &range, zs->dict);
  }
}

void IntervalVisitor::ActionRem(const zlexrangespec& range) {
  if (zobj_->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj_->ptr;
    unsigned long deleted = 0;
    zl = zzlDeleteRangeByLex(zl, &range, &deleted);
    zobj_->ptr = zl;
    removed_ = deleted;
  } else {
    CHECK_EQ(OBJ_ENCODING_SKIPLIST, zobj_->encoding);
    zset* zs = (zset*)zobj_->ptr;
    removed_ = zslDeleteRangeByLex(zs->zsl, &range, zs->dict);
  }
}

void IntervalVisitor::ExtractListPack(const zrangespec& range) {
  uint8_t* zl = (uint8_t*)zobj_->ptr;
  uint8_t *eptr, *sptr;
  uint8_t* vstr;
  unsigned int vlen = 0;
  long long vlong = 0;
  unsigned offset = params_.offset;
  unsigned limit = params_.limit;

  /* If reversed, get the last node in range as starting point. */
  if (params_.reverse) {
    eptr = zzlLastInRange(zl, &range);
  } else {
    eptr = zzlFirstInRange(zl, &range);
  }

  /* Get score pointer for the first element. */
  if (eptr)
    sptr = lpNext(zl, eptr);

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (eptr && offset--) {
    Next(zl, &eptr, &sptr);
  }

  while (eptr && limit--) {
    double score = zzlGetScore(sptr);

    /* Abort when the node is no longer in range. */
    if (!IsUnder(score, range))
      break;

    /* We know the element exists, so lpGetValue should always
     * succeed */
    vstr = lpGetValue(eptr, &vlen, &vlong);

    AddResult(vstr, vlen, vlong, score);

    /* Move to next node */
    Next(zl, &eptr, &sptr);
  }
}

void IntervalVisitor::ExtractSkipList(const zrangespec& range) {
  zset* zs = (zset*)zobj_->ptr;
  zskiplist* zsl = zs->zsl;
  zskiplistNode* ln;
  unsigned offset = params_.offset;
  unsigned limit = params_.limit;

  /* If reversed, get the last node in range as starting point. */
  if (params_.reverse) {
    ln = zslLastInRange(zsl, &range);
  } else {
    ln = zslFirstInRange(zsl, &range);
  }

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (ln && offset--) {
    ln = Next(ln);
  }

  while (ln && limit--) {
    /* Abort when the node is no longer in range. */
    if (!IsUnder(ln->score, range))
      break;

    result_.emplace_back(string{ln->ele, sdslen(ln->ele)}, ln->score);

    /* Move to next node */
    ln = Next(ln);
  }
}

void IntervalVisitor::ExtractListPack(const zlexrangespec& range) {
  uint8_t* zl = (uint8_t*)zobj_->ptr;
  uint8_t *eptr, *sptr = nullptr;
  uint8_t* vstr = nullptr;
  unsigned int vlen = 0;
  long long vlong = 0;
  unsigned offset = params_.offset;
  unsigned limit = params_.limit;

  /* If reversed, get the last node in range as starting point. */
  if (params_.reverse) {
    eptr = zzlLastInLexRange(zl, &range);
  } else {
    eptr = zzlFirstInLexRange(zl, &range);
  }

  /* Get score pointer for the first element. */
  if (eptr)
    sptr = lpNext(zl, eptr);

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (eptr && offset--) {
    Next(zl, &eptr, &sptr);
  }

  while (eptr && limit--) {
    double score = 0;
    if (params_.with_scores) /* don't bother to extract the score if it's gonna be ignored. */
      score = zzlGetScore(sptr);

    /* Abort when the node is no longer in range. */
    if (params_.reverse) {
      if (!zzlLexValueGteMin(eptr, &range))
        break;
    } else {
      if (!zzlLexValueLteMax(eptr, &range))
        break;
    }

    vstr = lpGetValue(eptr, &vlen, &vlong);
    AddResult(vstr, vlen, vlong, score);

    /* Move to next node */
    Next(zl, &eptr, &sptr);
  }
}

void IntervalVisitor::ExtractSkipList(const zlexrangespec& range) {
  zset* zs = (zset*)zobj_->ptr;
  zskiplist* zsl = zs->zsl;
  zskiplistNode* ln;
  unsigned offset = params_.offset;
  unsigned limit = params_.limit;

  /* If reversed, get the last node in range as starting point. */
  if (params_.reverse) {
    ln = zslLastInLexRange(zsl, &range);
  } else {
    ln = zslFirstInLexRange(zsl, &range);
  }

  /* If there is an offset, just traverse the number of elements without
   * checking the score because that is done in the next loop. */
  while (ln && offset--) {
    ln = Next(ln);
  }

  while (ln && limit--) {
    /* Abort when the node is no longer in range. */
    if (params_.reverse) {
      if (!zslLexValueGteMin(ln->ele, &range))
        break;
    } else {
      if (!zslLexValueLteMax(ln->ele, &range))
        break;
    }

    result_.emplace_back(string{ln->ele, sdslen(ln->ele)}, ln->score);

    /* Move to next node */
    ln = Next(ln);
  }
}

void IntervalVisitor::AddResult(const uint8_t* vstr, unsigned vlen, long long vlong, double score) {
  if (vstr == NULL) {
    result_.emplace_back(absl::StrCat(vlong), score);
  } else {
    result_.emplace_back(string{reinterpret_cast<const char*>(vstr), vlen}, score);
  }
}

bool ParseScore(string_view src, double* score) {
  if (src.empty())
    return false;

  if (src == "-inf") {
    *score = -HUGE_VAL;
  } else if (src == "+inf") {
    *score = HUGE_VAL;
  } else {
    absl::from_chars_result result = absl::from_chars(src.data(), src.end(), *score);
    if (int(result.ec) != 0 || result.ptr != src.end() || isnan(*score))
      return false;
  }
  return true;
};

bool ParseBound(string_view src, ZSetFamily::Bound* bound) {
  if (src.empty())
    return false;

  if (src[0] == '(') {
    bound->is_open = true;
    src.remove_prefix(1);
  }

  return ParseScore(src, &bound->val);
}

bool ParseLexBound(string_view src, ZSetFamily::LexBound* bound) {
  if (src.empty())
    return false;

  if (src == "+") {
    bound->type = ZSetFamily::LexBound::PLUS_INF;
  } else if (src == "-") {
    bound->type = ZSetFamily::LexBound::MINUS_INF;
  } else if (src[0] == '(') {
    bound->type = ZSetFamily::LexBound::OPEN;
    src.remove_prefix(1);
    bound->val = src;
  } else if (src[0] == '[') {
    bound->type = ZSetFamily::LexBound::CLOSED;
    src.remove_prefix(1);
    bound->val = src;
  } else {
    return false;
  }

  return true;
}

void SendAtLeastOneKeyError(ConnectionContext* cntx) {
  string name = cntx->cid->name();
  absl::AsciiStrToLower(&name);
  (*cntx)->SendError(absl::StrCat("at least 1 input key is needed for ", name));
}

enum class AggType : uint8_t { SUM, MIN, MAX };
using ScoredMap = absl::flat_hash_map<std::string, double>;

ScoredMap FromObject(const CompactObj& co, double weight) {
  robj* obj = co.AsRObj();
  ZSetFamily::RangeParams params;
  params.with_scores = true;
  IntervalVisitor vis(Action::RANGE, params, obj);
  vis(ZSetFamily::IndexInterval(0, -1));

  ZSetFamily::ScoredArray arr = vis.PopResult();
  ScoredMap res;
  res.reserve(arr.size());

  for (auto& elem : arr) {
    elem.second *= weight;
    res.emplace(move(elem));
  }

  return res;
}

double Aggregate(double v1, double v2, AggType atype) {
  switch (atype) {
    case AggType::SUM:
      return v1 + v2;
    case AggType::MAX:
      return max(v1, v2);
    case AggType::MIN:
      return min(v1, v2);
  }
  return 0;
}

// the result is in the destination.
void UnionScoredMap(ScoredMap* dest, ScoredMap* src, AggType agg_type) {
  ScoredMap* target = dest;
  ScoredMap* iter = src;

  if (iter->size() > target->size())
    swap(target, iter);

  for (const auto& elem : *iter) {
    auto [it, inserted] = target->emplace(elem);
    if (!inserted) {
      it->second = Aggregate(it->second, elem.second, agg_type);
    }
  }

  if (target != dest)
    dest->swap(*src);
}

void InterScoredMap(ScoredMap* dest, ScoredMap* src, AggType agg_type) {
  ScoredMap* target = dest;
  ScoredMap* iter = src;

  if (iter->size() > target->size())
    swap(target, iter);

  auto it = iter->begin();
  while (it != iter->end()) {
    auto inter_it = target->find(it->first);
    if (inter_it == target->end()) {
      auto copy_it = it++;
      iter->erase(copy_it);
    } else {
      it->second = Aggregate(it->second, inter_it->second, agg_type);
      ++it;
    }
  }

  if (iter != dest)
    dest->swap(*src);
}

OpResult<ScoredMap> OpUnion(EngineShard* shard, Transaction* t, string_view dest, AggType agg_type,
                            const vector<double>& weights, bool store) {
  ArgSlice keys = t->ShardArgsInShard(shard->shard_id());
  DVLOG(1) << "shard:" << shard->shard_id() << ", keys " << vector(keys.begin(), keys.end());
  DCHECK(!keys.empty());

  unsigned start = 0;

  if (keys.front() == dest) {
    ++start;
  }

  auto& db_slice = shard->db_slice();
  vector<pair<PrimeIterator, double>> it_arr(keys.size() - start);
  if (it_arr.empty())     // could be when only the dest key is hosted in this shard
    return OpStatus::OK;  // return empty map

  for (unsigned j = start; j < keys.size(); ++j) {
    auto it_res = db_slice.Find(t->db_index(), keys[j], OBJ_ZSET);
    if (it_res == OpStatus::WRONG_TYPE)  // TODO: support sets with default score 1.
      return it_res.status();
    if (!it_res)
      continue;

    // first global index is 2 after {destkey, numkeys}
    unsigned src_indx = j - start;
    unsigned windex = t->ReverseArgIndex(shard->shard_id(), j) - 2;
    DCHECK_LT(windex, weights.size());
    it_arr[src_indx] = {*it_res, weights[windex]};
  }

  ScoredMap result;
  for (auto it = it_arr.begin(); it != it_arr.end(); ++it) {
    if (it->first.is_done())
      continue;

    ScoredMap sm = FromObject(it->first->second, it->second);
    if (result.empty())
      result.swap(sm);
    else
      UnionScoredMap(&result, &sm, agg_type);
  }

  return result;
}

OpResult<ScoredMap> OpInter(EngineShard* shard, Transaction* t, string_view dest, AggType agg_type,
                            const vector<double>& weights, bool store) {
  ArgSlice keys = t->ShardArgsInShard(shard->shard_id());
  DVLOG(1) << "shard:" << shard->shard_id() << ", keys " << vector(keys.begin(), keys.end());
  DCHECK(!keys.empty());

  unsigned start = 0;

  if (keys.front() == dest) {
    ++start;
  }

  auto& db_slice = shard->db_slice();
  vector<pair<PrimeIterator, double>> it_arr(keys.size() - start);
  if (it_arr.empty())          // could be when only the dest key is hosted in this shard
    return OpStatus::SKIPPED;  // return noop

  for (unsigned j = start; j < keys.size(); ++j) {
    auto it_res = db_slice.Find(t->db_index(), keys[j], OBJ_ZSET);
    if (it_res == OpStatus::WRONG_TYPE)  // TODO: support sets with default score 1.
      return it_res.status();

    if (!it_res)
      continue;  // we exit in the next loop

    // first global index is 2 after {destkey, numkeys}
    unsigned src_indx = j - start;
    unsigned windex = t->ReverseArgIndex(shard->shard_id(), j) - 2;
    DCHECK_LT(windex, weights.size());
    it_arr[src_indx] = {*it_res, weights[windex]};
  }

  ScoredMap result;
  for (auto it = it_arr.begin(); it != it_arr.end(); ++it) {
    if (it->first.is_done()) {
      return ScoredMap{};
    }

    ScoredMap sm = FromObject(it->first->second, it->second);
    if (result.empty())
      result.swap(sm);
    else
      InterScoredMap(&result, &sm, agg_type);

    if (result.empty())
      return result;
  }

  return result;
}

using ScoredMemberView = std::pair<double, std::string_view>;
using ScoredMemberSpan = absl::Span<ScoredMemberView>;

struct AddResult {
  double new_score = 0;
  unsigned num_updated = 0;

  bool is_nan = false;
};

OpResult<AddResult> OpAdd(const OpArgs& op_args, const ZParams& zparams, string_view key,
                          ScoredMemberSpan members) {
  DCHECK(!members.empty() || zparams.override);
  auto& db_slice = op_args.shard->db_slice();

  if (zparams.override && members.empty()) {
    auto it = db_slice.FindExt(op_args.db_ind, key).first;
    db_slice.Del(op_args.db_ind, it);
    return OpStatus::OK;
  }

  OpResult<PrimeIterator> res_it = FindZEntry(zparams, op_args, key, members.front().second.size());

  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();

  unsigned added = 0;
  unsigned updated = 0;
  unsigned processed = 0;

  sds& tmp_str = op_args.shard->tmp_str1;
  double new_score = 0;
  int retflags = 0;

  OpStatus op_status = OpStatus::OK;
  AddResult aresult;

  for (size_t j = 0; j < members.size(); j++) {
    const auto& m = members[j];
    tmp_str = sdscpylen(tmp_str, m.second.data(), m.second.size());

    int retval = zsetAdd(zobj, m.first, tmp_str, zparams.flags, &retflags, &new_score);

    if (zparams.flags & ZADD_IN_INCR) {
      if (retval == 0) {
        CHECK_EQ(1u, members.size());

        aresult.is_nan = true;
        break;
      }

      if (retflags & ZADD_OUT_NOP) {
        op_status = OpStatus::SKIPPED;
      }
    }

    if (retflags & ZADD_OUT_ADDED)
      added++;
    if (retflags & ZADD_OUT_UPDATED)
      updated++;
    if (!(retflags & ZADD_OUT_NOP))
      processed++;
  }

  DVLOG(2) << "ZAdd " << zobj->ptr;

  res_it.value()->second.SyncRObj();
  op_args.shard->db_slice().PostUpdate(op_args.db_ind, *res_it);

  if (zparams.flags & ZADD_IN_INCR) {
    aresult.new_score = new_score;
  } else {
    aresult.num_updated = zparams.ch ? added + updated : added;
  }

  if (op_status != OpStatus::OK)
    return op_status;
  return aresult;
}

struct StoreArgs {
  AggType agg_type = AggType::SUM;
  unsigned num_keys;
  vector<double> weights;
};

OpResult<StoreArgs> ParseStoreArgs(CmdArgList args) {
  string_view num_str = ArgS(args, 2);
  StoreArgs store_args;

  // we parsed the structure before, when transaction has been initialized.
  CHECK(absl::SimpleAtoi(num_str, &store_args.num_keys));
  DCHECK_GE(args.size(), 3 + store_args.num_keys);

  store_args.weights.resize(store_args.num_keys, 1);
  for (size_t i = 3 + store_args.num_keys; i < args.size(); ++i) {
    ToUpper(&args[i]);
    string_view arg = ArgS(args, i);
    if (arg == "WEIGHTS") {
      if (args.size() <= i + store_args.num_keys) {
        return OpStatus::SYNTAX_ERR;
      }

      for (unsigned j = 0; j < store_args.num_keys; ++j) {
        string_view weight = ArgS(args, i + j + 1);
        if (!absl::SimpleAtod(weight, &store_args.weights[j])) {
          return OpStatus::INVALID_FLOAT;
        }
      }
      i += store_args.num_keys;
    } else if (arg == "AGGREGATE") {
      if (i + 2 != args.size()) {
        return OpStatus::SYNTAX_ERR;
      }

      ToUpper(&args[i + 1]);

      string_view agg = ArgS(args, i + 1);
      if (agg == "SUM") {
        store_args.agg_type = AggType::SUM;
      } else if (agg == "MIN") {
        store_args.agg_type = AggType::MIN;
      } else if (agg == "MAX") {
        store_args.agg_type = AggType::MAX;
      } else {
        return OpStatus::SYNTAX_ERR;
      }
      break;
    } else {
      return OpStatus::SYNTAX_ERR;
    }
  }

  return store_args;
};

}  // namespace

void ZSetFamily::ZAdd(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);

  ZParams zparams;
  size_t i = 2;
  for (; i < args.size() - 1; ++i) {
    ToUpper(&args[i]);

    string_view cur_arg = ArgS(args, i);

    if (cur_arg == "XX") {
      zparams.flags |= ZADD_IN_XX;  // update only
    } else if (cur_arg == "NX") {
      zparams.flags |= ZADD_IN_NX;  // add new only.
    } else if (cur_arg == "GT") {
      zparams.flags |= ZADD_IN_GT;
    } else if (cur_arg == "LT") {
      zparams.flags |= ZADD_IN_LT;
    } else if (cur_arg == "CH") {
      zparams.ch = true;
    } else if (cur_arg == "INCR") {
      zparams.flags |= ZADD_IN_INCR;
    } else {
      break;
    }
  }

  if ((args.size() - i) % 2 != 0) {
    (*cntx)->SendError(kSyntaxErr);
    return;
  }

  if ((zparams.flags & ZADD_IN_INCR) && (i + 2 < args.size())) {
    (*cntx)->SendError("INCR option supports a single increment-element pair");
    return;
  }

  unsigned insert_mask = zparams.flags & (ZADD_IN_NX | ZADD_IN_XX);
  if (insert_mask == (ZADD_IN_NX | ZADD_IN_XX)) {
    (*cntx)->SendError(kNxXxErr);
    return;
  }

  constexpr auto kRangeOpt = ZADD_IN_GT | ZADD_IN_LT;
  if (((zparams.flags & ZADD_IN_NX) && (zparams.flags & kRangeOpt)) ||
      ((zparams.flags & kRangeOpt) == kRangeOpt)) {
    (*cntx)->SendError("GT, LT, and/or NX options at the same time are not compatible");
    return;
  }

  absl::InlinedVector<ScoredMemberView, 4> members;
  for (; i < args.size(); i += 2) {
    string_view cur_arg = ArgS(args, i);
    double val = 0;

    if (!ParseScore(cur_arg, &val)) {
      VLOG(1) << "Bad score:" << cur_arg << "|";
      return (*cntx)->SendError(kInvalidFloatErr);
    }
    if (isnan(val)) {
      return (*cntx)->SendError(kScoreNaN);
    }
    string_view member = ArgS(args, i + 1);
    members.emplace_back(val, member);
  }
  DCHECK(cntx->transaction);

  absl::Span memb_sp{members.data(), members.size()};
  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpAdd(op_args, zparams, key, memb_sp);
  };

  OpResult<AddResult> add_result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (base::_in(add_result.status(), {OpStatus::WRONG_TYPE, OpStatus::OUT_OF_MEMORY})) {
    return (*cntx)->SendError(add_result.status());
  }

  // KEY_NOTFOUND may happen in case of XX flag.
  if (add_result.status() == OpStatus::KEY_NOTFOUND) {
    if (zparams.flags & ZADD_IN_INCR)
      (*cntx)->SendNull();
    else
      (*cntx)->SendLong(0);
  } else if (add_result.status() == OpStatus::SKIPPED) {
    (*cntx)->SendNull();
  } else if (add_result->is_nan) {
    (*cntx)->SendError(kScoreNaN);
  } else {
    if (zparams.flags & ZADD_IN_INCR) {
      (*cntx)->SendDouble(add_result->new_score);
    } else {
      (*cntx)->SendLong(add_result->num_updated);
    }
  }
}

void ZSetFamily::ZCard(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);

  auto cb = [&](Transaction* t, EngineShard* shard) -> OpResult<uint32_t> {
    OpResult<PrimeIterator> find_res = shard->db_slice().Find(t->db_index(), key, OBJ_ZSET);
    if (!find_res) {
      return find_res.status();
    }

    return zsetLength(find_res.value()->second.AsRObj());
  };

  OpResult<uint32_t> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
    return;
  }

  (*cntx)->SendLong(result.value());
}

void ZSetFamily::ZCount(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);

  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  ScoreInterval si;
  if (!ParseBound(min_s, &si.first) || !ParseBound(max_s, &si.second)) {
    return (*cntx)->SendError(kFloatRangeErr);
  }

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpCount(op_args, key, si);
  };

  OpResult<unsigned> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
  } else {
    (*cntx)->SendLong(*result);
  }
}

void ZSetFamily::ZIncrBy(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view score_arg = ArgS(args, 2);

  ScoredMemberView scored_member;
  scored_member.second = ArgS(args, 3);

  if (!absl::SimpleAtod(score_arg, &scored_member.first)) {
    VLOG(1) << "Bad score:" << score_arg << "|";
    return (*cntx)->SendError(kInvalidFloatErr);
  }

  if (isnan(scored_member.first)) {
    return (*cntx)->SendError(kScoreNaN);
  }

  ZParams zparams;
  zparams.flags = ZADD_IN_INCR;

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpAdd(op_args, zparams, key, ScoredMemberSpan{&scored_member, 1});
  };

  OpResult<AddResult> add_result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (add_result.status() == OpStatus::WRONG_TYPE) {
    return (*cntx)->SendError(kWrongTypeErr);
  }

  if (add_result.status() == OpStatus::SKIPPED) {
    return (*cntx)->SendNull();
  }

  if (add_result->is_nan) {
    return (*cntx)->SendError(kScoreNaN);
  }

  (*cntx)->SendDouble(add_result->new_score);
}

void ZSetFamily::ZInterStore(CmdArgList args, ConnectionContext* cntx) {
  string_view dest_key = ArgS(args, 1);
  OpResult<StoreArgs> store_args_res = ParseStoreArgs(args);

  if (!store_args_res) {
    switch (store_args_res.status()) {
      case OpStatus::INVALID_FLOAT:
        return (*cntx)->SendError("weight value is not a float", kSyntaxErrType);
      default:
        return (*cntx)->SendError(store_args_res.status());
    }
  }
  const auto& store_args = *store_args_res;
  if (store_args.num_keys == 0) {
    return SendAtLeastOneKeyError(cntx);
  }

  vector<OpResult<ScoredMap>> maps(shard_set->size(), OpStatus::SKIPPED);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    maps[shard->shard_id()] =
        OpInter(shard, t, dest_key, store_args.agg_type, store_args.weights, false);
    return OpStatus::OK;
  };

  cntx->transaction->Schedule();
  cntx->transaction->Execute(std::move(cb), false);

  ScoredMap result;
  for (auto& op_res : maps) {
    if (op_res.status() == OpStatus::SKIPPED)
      continue;

    if (!op_res)
      return (*cntx)->SendError(op_res.status());

    if (result.empty())
      result.swap(op_res.value());
    else
      InterScoredMap(&result, &op_res.value(), store_args.agg_type);
    if (result.empty())
      break;
  }

  ShardId dest_shard = Shard(dest_key, maps.size());
  AddResult add_result;
  vector<ScoredMemberView> smvec;
  for (const auto& elem : result) {
    smvec.emplace_back(elem.second, elem.first);
  }

  auto store_cb = [&](Transaction* t, EngineShard* shard) {
    if (shard->shard_id() == dest_shard) {
      ZParams zparams;
      zparams.override = true;
      add_result =
          OpAdd(OpArgs{shard, t->db_index()}, zparams, dest_key, ScoredMemberSpan{smvec}).value();
    }
    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(store_cb), true);

  (*cntx)->SendLong(smvec.size());
}

void ZSetFamily::ZLexCount(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);

  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  LexInterval li;
  if (!ParseLexBound(min_s, &li.first) || !ParseLexBound(max_s, &li.second)) {
    return (*cntx)->SendError(kLexRangeErr);
  }

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpLexCount(op_args, key, li);
  };

  OpResult<unsigned> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
  } else {
    (*cntx)->SendLong(*result);
  }
}

void ZSetFamily::ZRange(CmdArgList args, ConnectionContext* cntx) {
  ZRangeGeneric(std::move(args), false, cntx);
}

void ZSetFamily::ZRank(CmdArgList args, ConnectionContext* cntx) {
  ZRankGeneric(std::move(args), false, cntx);
}

void ZSetFamily::ZRevRange(CmdArgList args, ConnectionContext* cntx) {
  ZRangeGeneric(std::move(args), true, cntx);
}

void ZSetFamily::ZRevRangeByScore(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  RangeParams range_params;
  range_params.reverse = true;
  args.remove_prefix(4);

  if (!ParseRangeByScoreParams(args, &range_params)) {
    return (*cntx)->SendError(kSyntaxErr);
  }

  ZRangeByScoreInternal(key, min_s, max_s, range_params, cntx);
}

void ZSetFamily::ZRevRank(CmdArgList args, ConnectionContext* cntx) {
  ZRankGeneric(std::move(args), true, cntx);
}

void ZSetFamily::ZRangeByLex(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);
  uint32_t offset = 0;
  uint32_t count = kuint32max;

  if (args.size() > 4) {
    if (args.size() != 7)
      return (*cntx)->SendError(kSyntaxErr);

    ToUpper(&args[4]);
    if (ArgS(args, 4) != "LIMIT")
      return (*cntx)->SendError(kSyntaxErr);
    string_view os = ArgS(args, 5);
    string_view cs = ArgS(args, 6);
    if (!SimpleAtoi(os, &count) || !SimpleAtoi(cs, &count)) {
      return (*cntx)->SendError(kInvalidIntErr);
    }
  }

  LexInterval li;
  if (!ParseLexBound(min_s, &li.first) || !ParseLexBound(max_s, &li.second)) {
    return (*cntx)->SendError(kLexRangeErr);
  }

  ZRangeSpec range_spec;
  range_spec.params.offset = offset;
  range_spec.params.limit = count;
  range_spec.interval = li;

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpRange(range_spec, op_args, key);
  };

  OpResult<ScoredArray> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  OutputScoredArrayResult(result, range_spec.params, cntx);
}

void ZSetFamily::ZRangeByScore(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  RangeParams range_params;
  args.remove_prefix(4);

  if (!ParseRangeByScoreParams(args, &range_params)) {
    return (*cntx)->SendError(kSyntaxErr);
  }

  ZRangeByScoreInternal(key, min_s, max_s, range_params, cntx);
}

void ZSetFamily::ZRemRangeByRank(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  IndexInterval ii;
  if (!SimpleAtoi(min_s, &ii.first) || !SimpleAtoi(max_s, &ii.second)) {
    return (*cntx)->SendError(kInvalidIntErr);
  }

  ZRangeSpec range_spec;
  range_spec.interval = ii;
  ZRemRangeGeneric(key, range_spec, cntx);
}

void ZSetFamily::ZRemRangeByScore(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  ScoreInterval si;
  if (!ParseBound(min_s, &si.first) || !ParseBound(max_s, &si.second)) {
    return (*cntx)->SendError(kFloatRangeErr);
  }

  ZRangeSpec range_spec;

  range_spec.interval = si;

  ZRemRangeGeneric(key, range_spec, cntx);
}

void ZSetFamily::ZRemRangeByLex(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  LexInterval li;
  if (!ParseLexBound(min_s, &li.first) || !ParseLexBound(max_s, &li.second)) {
    return (*cntx)->SendError(kLexRangeErr);
  }

  ZRangeSpec range_spec;

  range_spec.interval = li;

  ZRemRangeGeneric(key, range_spec, cntx);
}

void ZSetFamily::ZRem(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);

  absl::InlinedVector<string_view, 8> members(args.size() - 2);
  for (size_t i = 2; i < args.size(); ++i) {
    members[i - 2] = ArgS(args, i);
  }

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpRem(op_args, key, members);
  };

  OpResult<unsigned> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
  } else {
    (*cntx)->SendLong(*result);
  }
}

void ZSetFamily::ZScore(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view member = ArgS(args, 2);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpScore(op_args, key, member);
  };

  OpResult<double> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
  } else if (!result) {
    (*cntx)->SendNull();
  } else {
    (*cntx)->SendDouble(*result);
  }
}

void ZSetFamily::ZScan(CmdArgList args, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view token = ArgS(args, 2);

  uint64_t cursor = 0;

  if (!absl::SimpleAtoi(token, &cursor)) {
    return (*cntx)->SendError("invalid cursor");
  }

  if (args.size() > 3) {
    return (*cntx)->SendError("scan options are not supported yet");
  }

  auto cb = [&](Transaction* t, EngineShard* shard) {
    return OpScan(OpArgs{shard, t->db_index()}, key, &cursor);
  };

  OpResult<StringVec> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() != OpStatus::WRONG_TYPE) {
    (*cntx)->StartArray(2);
    (*cntx)->SendSimpleString(absl::StrCat(cursor));
    (*cntx)->StartArray(result->size());
    for (const auto& k : *result) {
      (*cntx)->SendBulkString(k);
    }
  } else {
    (*cntx)->SendError(result.status());
  }
}

void ZSetFamily::ZUnionStore(CmdArgList args, ConnectionContext* cntx) {
  string_view dest_key = ArgS(args, 1);
  OpResult<StoreArgs> store_args_res = ParseStoreArgs(args);

  if (!store_args_res) {
    switch (store_args_res.status()) {
      case OpStatus::INVALID_FLOAT:
        return (*cntx)->SendError("weight value is not a float", kSyntaxErrType);
      default:
        return (*cntx)->SendError(store_args_res.status());
    }
  }
  const auto& store_args = *store_args_res;
  if (store_args.num_keys == 0) {
    return SendAtLeastOneKeyError(cntx);
  }

  vector<OpResult<ScoredMap>> maps(shard_set->size());

  auto cb = [&](Transaction* t, EngineShard* shard) {
    maps[shard->shard_id()] =
        OpUnion(shard, t, dest_key, store_args.agg_type, store_args.weights, false);
    return OpStatus::OK;
  };

  cntx->transaction->Schedule();

  cntx->transaction->Execute(std::move(cb), false);
  ScoredMap result;

  for (auto& op_res : maps) {
    if (!op_res)
      return (*cntx)->SendError(op_res.status());
    UnionScoredMap(&result, &op_res.value(), store_args.agg_type);
  }
  ShardId dest_shard = Shard(dest_key, maps.size());
  AddResult add_result;
  vector<ScoredMemberView> smvec;
  for (const auto& elem : result) {
    smvec.emplace_back(elem.second, elem.first);
  }

  auto store_cb = [&](Transaction* t, EngineShard* shard) {
    if (shard->shard_id() == dest_shard) {
      ZParams zparams;
      zparams.override = true;
      add_result =
          OpAdd(OpArgs{shard, t->db_index()}, zparams, dest_key, ScoredMemberSpan{smvec}).value();
    }
    return OpStatus::OK;
  };

  cntx->transaction->Execute(std::move(store_cb), true);

  (*cntx)->SendLong(smvec.size());
}

void ZSetFamily::ZRangeByScoreInternal(string_view key, string_view min_s, string_view max_s,
                                       const RangeParams& params, ConnectionContext* cntx) {
  ZRangeSpec range_spec;
  range_spec.params = params;

  ScoreInterval si;
  if (!ParseBound(min_s, &si.first) || !ParseBound(max_s, &si.second)) {
    return (*cntx)->SendError(kFloatRangeErr);
  }
  range_spec.interval = si;

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpRange(range_spec, op_args, key);
  };

  OpResult<ScoredArray> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  OutputScoredArrayResult(result, params, cntx);
}

void ZSetFamily::OutputScoredArrayResult(const OpResult<ScoredArray>& result,
                                         const RangeParams& params, ConnectionContext* cntx) {
  if (result.status() == OpStatus::WRONG_TYPE) {
    return (*cntx)->SendError(kWrongTypeErr);
  }

  LOG_IF(WARNING, !result && result.status() != OpStatus::KEY_NOTFOUND)
      << "Unexpected status " << result.status();

  (*cntx)->StartArray(result->size() * (params.with_scores ? 2 : 1));
  const ScoredArray& array = result.value();
  for (const auto& p : array) {
    (*cntx)->SendBulkString(p.first);

    if (params.with_scores) {
      (*cntx)->SendDouble(p.second);
    }
  }
}

void ZSetFamily::ZRemRangeGeneric(string_view key, const ZRangeSpec& range_spec,
                                  ConnectionContext* cntx) {
  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpRemRange(op_args, key, range_spec);
  };

  OpResult<unsigned> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result.status() == OpStatus::WRONG_TYPE) {
    (*cntx)->SendError(kWrongTypeErr);
  } else {
    (*cntx)->SendLong(*result);
  }
}

void ZSetFamily::ZRangeGeneric(CmdArgList args, bool reverse, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view min_s = ArgS(args, 2);
  string_view max_s = ArgS(args, 3);

  bool parse_score = false;
  RangeParams range_params;
  range_params.reverse = reverse;

  for (size_t i = 4; i < args.size(); ++i) {
    ToUpper(&args[i]);

    string_view cur_arg = ArgS(args, i);
    if (!reverse && cur_arg == "BYSCORE") {
      parse_score = true;
    } else if (cur_arg == "WITHSCORES") {
      range_params.with_scores = true;
    } else {
      return cntx->reply_builder()->SendError(absl::StrCat("unsupported option ", cur_arg));
    }
  }

  if (parse_score) {
    ZRangeByScoreInternal(key, min_s, max_s, range_params, cntx);
    return;
  }

  IndexInterval ii;

  if (!SimpleAtoi(min_s, &ii.first) || !SimpleAtoi(max_s, &ii.second)) {
    (*cntx)->SendError(kInvalidIntErr);
    return;
  }

  ZRangeSpec range_spec;
  range_spec.params = range_params;
  range_spec.interval = ii;

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};
    return OpRange(range_spec, op_args, key);
  };

  OpResult<ScoredArray> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  OutputScoredArrayResult(result, range_params, cntx);
}

void ZSetFamily::ZRankGeneric(CmdArgList args, bool reverse, ConnectionContext* cntx) {
  string_view key = ArgS(args, 1);
  string_view member = ArgS(args, 2);

  auto cb = [&](Transaction* t, EngineShard* shard) {
    OpArgs op_args{shard, t->db_index()};

    return OpRank(op_args, key, member, reverse);
  };

  OpResult<unsigned> result = cntx->transaction->ScheduleSingleHopT(std::move(cb));
  if (result) {
    (*cntx)->SendLong(*result);
  } else if (result.status() == OpStatus::KEY_NOTFOUND) {
    (*cntx)->SendNull();
  } else {
    (*cntx)->SendError(result.status());
  }
}

bool ZSetFamily::ParseRangeByScoreParams(CmdArgList args, RangeParams* params) {
  for (size_t i = 0; i < args.size(); ++i) {
    ToUpper(&args[i]);

    string_view cur_arg = ArgS(args, i);
    if (cur_arg == "WITHSCORES") {
      params->with_scores = true;
    } else if (cur_arg == "LIMIT") {
      if (i + 3 != args.size())
        return false;

      string_view os = ArgS(args, i + 1);
      string_view cs = ArgS(args, i + 2);

      if (!SimpleAtoi(os, &params->offset) || !SimpleAtoi(cs, &params->limit))
        return false;
      i += 3;
    } else {
      return false;
    }
  }

  return true;
}

OpResult<StringVec> ZSetFamily::OpScan(const OpArgs& op_args, std::string_view key,
                                       uint64_t* cursor) {
  OpResult<PrimeIterator> find_res = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);

  if (!find_res)
    return find_res.status();

  PrimeIterator it = find_res.value();
  StringVec res;
  robj* zobj = it->second.AsRObj();
  char buf[128];

  if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
    RangeParams params;
    IntervalVisitor iv{Action::RANGE, params, zobj};

    iv(IndexInterval{0, kuint32max});
    ScoredArray arr = iv.PopResult();
    res.resize(arr.size() * 2);

    for (size_t i = 0; i < arr.size(); ++i) {
      char* str = RedisReplyBuilder::FormatDouble(arr[i].second, buf, sizeof(buf));

      res[2 * i] = std::move(arr[i].first);
      res[2 * i + 1].assign(str);
    }
    *cursor = 0;
  } else {
    CHECK_EQ(unsigned(OBJ_ENCODING_SKIPLIST), zobj->encoding);
    uint32_t count = 20;
    zset* zs = (zset*)zobj->ptr;

    dict* ht = zs->dict;
    long maxiterations = count * 10;

    struct ScanArgs {
      char* sbuf;
      StringVec* res;
    } sargs = {buf, &res};

    auto scanCb = [](void* privdata, const dictEntry* de) {
      ScanArgs* sargs = (ScanArgs*)privdata;

      sds key = (sds)de->key;
      double score = *(double*)dictGetVal(de);

      sargs->res->emplace_back(key, sdslen(key));
      char* str = RedisReplyBuilder::FormatDouble(score, sargs->sbuf, sizeof(buf));
      sargs->res->emplace_back(str);
    };

    do {
      *cursor = dictScan(ht, *cursor, scanCb, NULL, &sargs);
    } while (*cursor && maxiterations-- && res.size() < count);
  }

  return res;
}

OpResult<unsigned> ZSetFamily::OpRem(const OpArgs& op_args, string_view key, ArgSlice members) {
  auto& db_slice = op_args.shard->db_slice();
  OpResult<PrimeIterator> res_it = db_slice.Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  db_slice.PreUpdate(op_args.db_ind, *res_it);
  robj* zobj = res_it.value()->second.AsRObj();
  sds& tmp_str = op_args.shard->tmp_str1;
  unsigned deleted = 0;
  for (string_view member : members) {
    tmp_str = sdscpylen(tmp_str, member.data(), member.size());
    deleted += zsetDel(zobj, tmp_str);
  }
  auto zlen = zsetLength(zobj);
  res_it.value()->second.SyncRObj();
  db_slice.PostUpdate(op_args.db_ind, *res_it);

  if (zlen == 0) {
    CHECK(op_args.shard->db_slice().Del(op_args.db_ind, res_it.value()));
  }

  return deleted;
}

OpResult<double> ZSetFamily::OpScore(const OpArgs& op_args, string_view key, string_view member) {
  OpResult<PrimeIterator> res_it = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();
  sds& tmp_str = op_args.shard->tmp_str1;
  tmp_str = sdscpylen(tmp_str, member.data(), member.size());
  double score;
  int retval = zsetScore(zobj, tmp_str, &score);
  if (retval != C_OK) {
    return OpStatus::KEY_NOTFOUND;
  }
  return score;
}

auto ZSetFamily::OpRange(const ZRangeSpec& range_spec, const OpArgs& op_args, string_view key)
    -> OpResult<ScoredArray> {
  OpResult<PrimeIterator> res_it = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();
  IntervalVisitor iv{Action::RANGE, range_spec.params, zobj};

  std::visit(iv, range_spec.interval);

  return iv.PopResult();
}

OpResult<unsigned> ZSetFamily::OpRemRange(const OpArgs& op_args, string_view key,
                                          const ZRangeSpec& range_spec) {
  auto& db_slice = op_args.shard->db_slice();
  OpResult<PrimeIterator> res_it = db_slice.Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  db_slice.PreUpdate(op_args.db_ind, *res_it);

  robj* zobj = res_it.value()->second.AsRObj();

  IntervalVisitor iv{Action::REMOVE, range_spec.params, zobj};
  std::visit(iv, range_spec.interval);

  res_it.value()->second.SyncRObj();
  db_slice.PostUpdate(op_args.db_ind, *res_it);

  auto zlen = zsetLength(zobj);
  if (zlen == 0) {
    CHECK(op_args.shard->db_slice().Del(op_args.db_ind, res_it.value()));
  }

  return iv.removed();
}

OpResult<unsigned> ZSetFamily::OpRank(const OpArgs& op_args, string_view key, string_view member,
                                      bool reverse) {
  OpResult<PrimeIterator> res_it = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();
  op_args.shard->tmp_str1 = sdscpylen(op_args.shard->tmp_str1, member.data(), member.size());

  long res = zsetRank(zobj, op_args.shard->tmp_str1, reverse);
  if (res < 0)
    return OpStatus::KEY_NOTFOUND;
  return res;
}

OpResult<unsigned> ZSetFamily::OpCount(const OpArgs& op_args, std::string_view key,
                                       const ScoreInterval& interval) {
  OpResult<PrimeIterator> res_it = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();
  zrangespec range = GetZrangeSpec(false, interval);
  unsigned count = 0;

  if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj->ptr;
    uint8_t *eptr, *sptr;
    double score;

    /* Use the first element in range as the starting point */
    eptr = zzlFirstInRange(zl, &range);

    /* No "first" element */
    if (eptr == NULL) {
      return 0;
    }

    /* First element is in range */
    sptr = lpNext(zl, eptr);
    score = zzlGetScore(sptr);

    DCHECK(zslValueLteMax(score, &range));

    /* Iterate over elements in range */
    while (eptr) {
      score = zzlGetScore(sptr);

      /* Abort when the node is no longer in range. */
      if (!zslValueLteMax(score, &range)) {
        break;
      } else {
        count++;
        zzlNext(zl, &eptr, &sptr);
      }
    }
  } else {
    CHECK_EQ(unsigned(OBJ_ENCODING_SKIPLIST), zobj->encoding);
    zset* zs = (zset*)zobj->ptr;
    zskiplist* zsl = zs->zsl;
    zskiplistNode* zn;
    unsigned long rank;

    /* Find first element in range */
    zn = zslFirstInRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn == NULL)
      return 0;

    rank = zslGetRank(zsl, zn->score, zn->ele);
    count = (zsl->length - (rank - 1));

    /* Find last element in range */
    zn = zslLastInRange(zsl, &range);

    /* Use rank of last element, if any, to determine the actual count */
    if (zn != NULL) {
      rank = zslGetRank(zsl, zn->score, zn->ele);
      count -= (zsl->length - rank);
    }
  }

  return count;
}

OpResult<unsigned> ZSetFamily::OpLexCount(const OpArgs& op_args, string_view key,
                                          const ZSetFamily::LexInterval& interval) {
  OpResult<PrimeIterator> res_it = op_args.shard->db_slice().Find(op_args.db_ind, key, OBJ_ZSET);
  if (!res_it)
    return res_it.status();

  robj* zobj = res_it.value()->second.AsRObj();
  zlexrangespec range = GetLexRange(false, interval);
  unsigned count = 0;
  if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
    uint8_t* zl = (uint8_t*)zobj->ptr;
    uint8_t *eptr, *sptr;

    /* Use the first element in range as the starting point */
    eptr = zzlFirstInLexRange(zl, &range);

    /* No "first" element */
    if (eptr) {
      /* First element is in range */
      sptr = lpNext(zl, eptr);
      serverAssertWithInfo(c, zobj, zzlLexValueLteMax(eptr, &range));

      /* Iterate over elements in range */
      while (eptr) {
        /* Abort when the node is no longer in range. */
        if (!zzlLexValueLteMax(eptr, &range)) {
          break;
        } else {
          count++;
          zzlNext(zl, &eptr, &sptr);
        }
      }
    }
  } else {
    DCHECK_EQ(OBJ_ENCODING_SKIPLIST, zobj->encoding);
    zset* zs = (zset*)zobj->ptr;
    zskiplist* zsl = zs->zsl;
    zskiplistNode* zn;
    unsigned long rank;

    /* Find first element in range */
    zn = zslFirstInLexRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    if (zn != NULL) {
      rank = zslGetRank(zsl, zn->score, zn->ele);
      count = (zsl->length - (rank - 1));

      /* Find last element in range */
      zn = zslLastInLexRange(zsl, &range);

      /* Use rank of last element, if any, to determine the actual count */
      if (zn != NULL) {
        rank = zslGetRank(zsl, zn->score, zn->ele);
        count -= (zsl->length - rank);
      }
    }
  }

  zslFreeLexRange(&range);
  return count;
}

#define HFUNC(x) SetHandler(&ZSetFamily::x)

void ZSetFamily::Register(CommandRegistry* registry) {
  constexpr uint32_t kUnionMask = CO::WRITE | CO::VARIADIC_KEYS | CO::REVERSE_MAPPING;

  *registry << CI{"ZADD", CO::FAST | CO::WRITE | CO::DENYOOM, -4, 1, 1, 1}.HFUNC(ZAdd)
            << CI{"ZCARD", CO::FAST | CO::READONLY, 2, 1, 1, 1}.HFUNC(ZCard)
            << CI{"ZCOUNT", CO::FAST | CO::READONLY, 4, 1, 1, 1}.HFUNC(ZCount)
            << CI{"ZINCRBY", CO::FAST | CO::WRITE | CO::DENYOOM, 4, 1, 1, 1}.HFUNC(ZIncrBy)
            << CI{"ZINTERSTORE", kUnionMask, -4, 3, 3, 1}.HFUNC(ZInterStore)
            << CI{"ZLEXCOUNT", CO::READONLY, 4, 1, 1, 1}.HFUNC(ZLexCount)
            << CI{"ZREM", CO::FAST | CO::WRITE, -3, 1, 1, 1}.HFUNC(ZRem)
            << CI{"ZRANGE", CO::READONLY, -4, 1, 1, 1}.HFUNC(ZRange)
            << CI{"ZRANK", CO::READONLY | CO::FAST, 3, 1, 1, 1}.HFUNC(ZRank)
            << CI{"ZRANGEBYLEX", CO::READONLY, -4, 1, 1, 1}.HFUNC(ZRangeByLex)
            << CI{"ZRANGEBYSCORE", CO::READONLY, -4, 1, 1, 1}.HFUNC(ZRangeByScore)
            << CI{"ZSCORE", CO::READONLY | CO::FAST, 3, 1, 1, 1}.HFUNC(ZScore)
            << CI{"ZREMRANGEBYRANK", CO::WRITE, 4, 1, 1, 1}.HFUNC(ZRemRangeByRank)
            << CI{"ZREMRANGEBYSCORE", CO::WRITE, 4, 1, 1, 1}.HFUNC(ZRemRangeByScore)
            << CI{"ZREMRANGEBYLEX", CO::WRITE, 4, 1, 1, 1}.HFUNC(ZRemRangeByLex)
            << CI{"ZREVRANGE", CO::READONLY, -4, 1, 1, 1}.HFUNC(ZRevRange)
            << CI{"ZREVRANGEBYSCORE", CO::READONLY, -4, 1, 1, 1}.HFUNC(ZRevRangeByScore)
            << CI{"ZREVRANK", CO::READONLY | CO::FAST, 3, 1, 1, 1}.HFUNC(ZRevRank)
            << CI{"ZSCAN", CO::READONLY, -3, 1, 1, 1}.HFUNC(ZScan)
            << CI{"ZUNIONSTORE", kUnionMask, -4, 3, 3, 1}.HFUNC(ZUnionStore);
}

}  // namespace dfly
