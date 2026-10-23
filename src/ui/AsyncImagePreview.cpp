#include "ui/AsyncImagePreview.h"
#include <QtConcurrent>
namespace compositor {
AsyncImagePreview::AsyncImagePreview(QObject *parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<QImage>::finished, this, [this] {
        active_ = false;
        auto completion = std::move(runningCompletion_);
        if (runningGeneration_ == generation_ && completion)
            completion(watcher_.result());
        if (pending_ && !active_)
            start();
    });
}
void AsyncImagePreview::request(std::function<QImage()> work, std::function<void(QImage)> completed) {
    ++generation_;
    pending_ = std::move(work);
    pendingCompletion_ = std::move(completed);
    if (!active_)
        start();
}
void AsyncImagePreview::cancel() {
    ++generation_;
    pending_ = {};
    pendingCompletion_ = {};
    runningCompletion_ = {};
}
void AsyncImagePreview::start() {
    active_ = true;
    runningGeneration_ = generation_;
    runningCompletion_ = std::move(pendingCompletion_);
    auto work = std::move(pending_);
    pending_ = {};
    watcher_.setFuture(QtConcurrent::run([work = std::move(work)] {
        try {
            return work();
        } catch (...) {
            return QImage();
        }
    }));
}
} // namespace compositor
