#pragma once
#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <functional>

namespace compositor {
// One running job plus the newest pending request. Workers own immutable inputs;
// callbacks run on the GUI thread and stale/cancelled results never reach the document.
class AsyncImagePreview final : public QObject {
  public:
    explicit AsyncImagePreview(QObject *parent = nullptr);
    void request(std::function<QImage()> work, std::function<void(QImage)> completed);
    void cancel();
    bool busy() const { return active_; }

  private:
    void start();
    bool active_ = false;
    QFutureWatcher<QImage> watcher_;
    quint64 generation_ = 0, runningGeneration_ = 0;
    std::function<QImage()> pending_;
    std::function<void(QImage)> pendingCompletion_, runningCompletion_;
};
} // namespace compositor
