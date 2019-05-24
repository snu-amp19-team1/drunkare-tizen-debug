#include <vector>
#include <deque>
#include <iostream>
#include <atomic>
#include <mutex>
#include <memory>
#include <condition_variable>

// Concurrent FIFO queue
template <typename T>
struct Queue {
  Queue() : _done(false) {};

  std::unique_ptr<T> dequeue() {
    std::unique_lock<std::mutex> lk(m);
    bool done;
    cv.wait(lk, [this, &done]() {
      done = this->_done.load();
      return !this->container.empty() || done;
    });

    if (done) {
      lk.unlock();
      return nullptr;
    }

    auto result = std::move(container.front());
    container.pop_front();
    lk.unlock();
    return result;
  }

  void enqueue(std::unique_ptr<T> data) {
    std::unique_lock<std::mutex> lk(m);
    container.push_back(std::move(data));
    lk.unlock();
    cv.notify_all();
  }

  void forceDone() {
    _done.store(true);
    cv.notify_all();
  }

  void clear() {
    container.clear();
    _done.store(false);
    cv.notify_all();
  }

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> _done;
  std::deque<std::unique_ptr<T>> container;
};
