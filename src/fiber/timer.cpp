#include "timer.hpp"
#include "utils.hpp"

namespace monsoon {
//排序规则,谁小谁在前面
bool Timer::Comparator::operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const {
  if (!lhs && !rhs) {
    return false;
  }
  if (!lhs) {
    return true;
  }
  if (!rhs) {
    return false;
  }
  if (lhs->next_ < rhs->next_) {
    return true;
  }
  if (rhs->next_ < lhs->next_) {
    return false;
  }
  return lhs.get() < rhs.get();
}


Timer::Timer(uint64_t ms, std::function<void()> cb, bool recuring, TimerManager *manager)
    : recurring_(recuring), ms_(ms), cb_(cb), manager_(manager) {
  next_ = GetElapsedMS() + ms_; //下次触发的绝对时间戳
}

Timer::Timer(uint64_t next) : next_(next) {}
bool Timer::cancel() {
  RWMutex::WriteLock lock(manager_->mutex_);
  if (cb_) {
    cb_ = nullptr;
    auto it = manager_->timers_.find(shared_from_this());
    manager_->timers_.erase(it);
    return true;
  }
  return false;
}
//更新定时器时间？
bool Timer::refresh() {
  RWMutex::RWMutex::WriteLock lock(manager_->mutex_);
  if (!cb_) {
    return false;
  }
  auto it = manager_->timers_.find(shared_from_this());
  if (it == manager_->timers_.end()) {
    return false;
  }
  //重新排序，timers是一个set集合，底层红黑树
  manager_->timers_.erase(it);
  next_ = GetElapsedMS() + ms_;
  manager_->timers_.insert(shared_from_this());
  return true;
}

// 重置定时器，重新设置定时器触发时间
// from_now = true: 下次出发时间从当前时刻开始计算
// from_now = false: 下次触发时间从上一次开始计算
bool Timer::reset(uint64_t ms, bool from_now) {
  if (ms == ms_ && !from_now) {
    return true;
  }
  RWMutex::WriteLock lock(manager_->mutex_);
  if (!cb_) {
    return true;
  }
  auto it = manager_->timers_.find(shared_from_this()); //如何返回哪一个ptr？就是调用该函数的对象
  if (it == manager_->timers_.end()) {
    return false;
  }
  manager_->timers_.erase(it);
  uint64_t start = 0;
  if (from_now) {
    start = GetElapsedMS();
  } else {
    start = next_ - ms_;
  }
  ms_ = ms;
  next_ = start + ms_;
  manager_->addTimer(shared_from_this(), lock);
  return true;
}

TimerManager::TimerManager() { previouseTime_ = GetElapsedMS(); } //基准时间戳

TimerManager::~TimerManager() {}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb, bool recurring) {
  Timer::ptr timer(new Timer(ms, cb, recurring, this));
  RWMutex::WriteLock lock(mutex_);
  addTimer(timer, lock);
  return timer;
}

//有时候希望定时器的回调只在某个“条件对象”还存活时才执行，为什么是void？
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
  std::shared_ptr<void> tmp = weak_cond.lock();
  if (tmp) {
    cb();
  }
}
Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond,
                                           bool recurring) {
  return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}


uint64_t TimerManager::getNextTimer() {
  RWMutex::ReadLock lock(mutex_);
  tickled_ = false; //？避免重复唤醒。
  if (timers_.empty()) {
    return ~0ull;
  }
  const Timer::ptr &next = *timers_.begin(); //timers_ 是按 next_（触发时间）升序排列的红黑树，begin() 就是下一个要执行的定时器。
  uint64_t now_ms = GetElapsedMS();
  //如果当前时间已经不小于最早定时器的触发时刻，说明它已经到期（甚至过去了），此时返回 0，让调度线程立即执行 listExpiredCb()。
  //否则返回 next->next_ - now_ms，即距离下次触发还剩下多少毫秒，调度线程可以以此作为超时参数，等待该时长后再唤醒。
  if (now_ms >= next->next_) {
    return 0;
  } else {
    return next->next_ - now_ms;
  }
}

//检测是否有到期任务，没有注意到
void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
  uint64_t now_ms = GetElapsedMS();
  std::vector<Timer::ptr> expired;
  {
    RWMutex::ReadLock lock(mutex_);
    if (timers_.empty()) {
      return;
    }
  }
  RWMutex::WriteLock lock(mutex_);
  if (timers_.empty()) {
    return;
  }
  bool rollover = false;
  if (detectClockRolllover(now_ms)) { 
//作用就是检测「单调时钟」（GetElapsedMS()）是否出现了 回拨（roll-over），也就是时间戳意外地向前跳了很大一段，从而有可能让定时器永远等不到过期。
    rollover = true;
  }
  if (!rollover && ((*timers_.begin())->next_ > now_ms)) {
    return;
  }

  Timer::ptr now_timer(new Timer(now_ms));
  auto it = rollover ? timers_.end() : timers_.lower_bound(now_timer);
  while (it != timers_.end() && (*it)->next_ == now_ms) {
    ++it;
  }
  expired.insert(expired.begin(), timers_.begin(), it);
  timers_.erase(timers_.begin(), it);

  cbs.reserve(expired.size());
  for (auto &timer : expired) {
    cbs.push_back(timer->cb_);
    if (timer->recurring_) {
      // 循环计时，重新加入堆中
      timer->next_ = now_ms + timer->ms_;
      timers_.insert(timer);
    } else {
      timer->cb_ = nullptr;
    }
  }
}

void TimerManager::addTimer(Timer::ptr val, RWMutex::WriteLock &lock) {
    // 1. 插入红黑树，得到插入位置的迭代器
  auto it = timers_.insert(val).first;
  bool at_front = (it == timers_.begin()) && !tickled_;
  if (at_front) {
    tickled_ = true;
  }
  lock.unlock();
  if (at_front) {
    OnTimerInsertedAtFront();
  }
}

bool TimerManager::detectClockRolllover(uint64_t now_ms) {
  bool rollover = false;
  if (now_ms < previouseTime_ && now_ms < (previouseTime_ - 60 * 60 * 1000)) {
    rollover = true;
  }
  previouseTime_ = now_ms;
  return rollover;
}

bool TimerManager::hasTimer() {
  RWMutex::ReadLock lock(mutex_);
  return !timers_.empty();
}

}  // namespace monsoon