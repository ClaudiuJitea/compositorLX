#!/usr/bin/env bash
# GPU test passes for machines without a GPU (D6): the renderer suites on software Vulkan (lavapipe), under the Khronos
# validation layer with synchronization checks, and with forced staging. Usage: scripts/ci-gpu-tests.sh <build-dir>
set -euo pipefail
build=${1:-build}
lavapipe=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -n1)
if [[ -z "$lavapipe" ]]; then
    echo "lavapipe (mesa-vulkan-drivers) is not installed" >&2
    exit 1
fi
export COMPOSITOR_GPU_ALLOW_SOFTWARE=1 COMPOSITOR_REQUIRE_VULKAN_TESTS=1 VK_DRIVER_FILES="$lavapipe"
export COMPOSITOR_GPU_NO_PIPELINE_CACHE=1 QT_QPA_PLATFORM=offscreen
run() {
    echo "== $*"
    for suite in gpu_renderer_tests gpu_document_tests rgb_split_gpu_tests lens_effects_gpu_tests; do
        "$build/tests/$suite" 2>&1 | tee -a "$log" | grep -E '^Totals' | sed "s/^/$suite: /"
    done
}
log=$(mktemp)
run "lavapipe"
COMPOSITOR_GPU_FORCE_STAGING=1 run "lavapipe, forced staging"
layers=${VK_LAYER_PATH:-/usr/share/vulkan/explicit_layer.d}
if [[ -f "$layers/VkLayer_khronos_validation.json" ]]; then
    : >"$log"
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 run "validation layer"
    errors=$(grep -c "Validation Error" "$log" || true)
    echo "validation errors: $errors"
    [[ "$errors" == 0 ]] || exit 1
else
    echo "Khronos validation layer not installed; skipped"
fi
