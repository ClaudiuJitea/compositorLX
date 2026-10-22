#pragma once
#include "core/EditorSession.h"
#include <QWidget>
#include <functional>
class QListWidget;
class QPushButton;
class QDoubleSpinBox;
class QCheckBox;
class QTimer;
namespace compositor {
class GifTimeline : public QWidget {
public:
    GifTimeline(EditorSession &session, std::function<void()> changed, QWidget *parent = nullptr);
    void refresh(bool canvasOnly = false);
    void stop();
private:
    EditorSession &session_;
    std::function<void()> changed_;
    QListWidget *frames_;
    QPushButton *play_, *remove_;
    QDoubleSpinBox *delay_;
    QCheckBox *same_;
    QTimer *timer_;
    QUuid layerId_;
    bool refreshing_ = false;
};
}
