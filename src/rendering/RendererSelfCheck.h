#pragma once
#include <QJsonObject>
namespace compositor {
// Renders a small exposure + blur graph on Vulkan and compares it with the CPU reference (`--check-renderer` and the
// Rendering dialog). "valid" is true when the GPU result is within one level, or when the CPU fallback is in use.
QJsonObject rendererSelfCheck();
} // namespace compositor
