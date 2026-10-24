#pragma once
#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace compositor {
// GIF playback's schedule (macOS GIFPreviewPlayback): every frame shows from its own start in the loop, measured from
// when playback began, so the time spent decoding a frame never lengthens the delays saved with the layer. A frame
// whose time has already passed while the last one decoded is skipped. Times are microseconds from the start; the
// caller sleeps until `next()`, then asks `advance` which frame to show with the clock read before decoding.
class GifPlayback {
  public:
    struct Step {
        int show = -1;         // the frame position to show now, or -1 to keep the one on screen
        bool finished = false; // playing once, the end was reached
    };
    GifPlayback() = default;
    GifPlayback(const QVector<qint64> &durations, int initial, bool loops = true) : loops_(loops)
    {
        if (durations.size() < 2 || initial < 0 || initial >= durations.size()) return;
        for (qint64 duration : durations) {
            if (duration <= 0) return;
            starts_.append(total_);
            total_ += duration;
        }
        offset_ = starts_[initial];
        displayed_ = initial;
        next_ = durations[initial];
        valid_ = true;
    }
    [[nodiscard]] bool valid() const { return valid_; }
    // When to wake next, in microseconds from the start.
    [[nodiscard]] qint64 next() const { return next_; }
    [[nodiscard]] int displayed() const { return displayed_; }

    Step advance(qint64 elapsed)
    {
        Step step;
        if (!valid_) return {-1, true};
        const qint64 position = offset_ + elapsed;
        if (!loops_ && position >= total_) {
            // Once always ends on the last frame, even if decoding missed its interval.
            if (displayed_ != starts_.size() - 1) step.show = displayed_ = int(starts_.size() - 1);
            step.finished = true;
            valid_ = false;
            return step;
        }
        qint64 time = position % total_;
        if (time < 0 || time >= total_) time = 0;
        const int index = int(std::upper_bound(starts_.cbegin(), starts_.cend(), time) - starts_.cbegin()) - 1;
        const qint64 end = index + 1 < starts_.size() ? starts_[index + 1] : total_;
        // Keep the boundary sampled before decoding. Late frames catch up on the next wake-up.
        next_ = elapsed + end - time;
        if (index != displayed_) step.show = displayed_ = std::max(0, index);
        return step;
    }

  private:
    QVector<qint64> starts_;
    qint64 total_ = 0, offset_ = 0, next_ = 0;
    int displayed_ = 0;
    bool loops_ = true, valid_ = false;
};
} // namespace compositor
