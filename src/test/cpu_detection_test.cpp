// CPU Detection System Test
//
// Tests the 3-layer CPU detection architecture with ARM64 optimization

#include "framework/moss_ut.hpp"
import moss.kernel;
import moss.logging;

using namespace moss::kernel::cpu_detection;
using namespace moss::kernel::cpu_topology;

class CpuDetectionTest : public TestCase {
public:
  void test_detection_layers() {
    moss::kernel::logging::klog::info("=== Testing 3-Layer CPU Detection Architecture ===");

    // Initialize CPU detection system
    initialize_cpu_detection();

    // Test Layer 1: FDT Detection
    FdtCpuDetector fdt_detector;
    auto fdt_result = fdt_detector.detect();

    assert_true(fdt_result.cpu_count >= 1, "FDT detection should return at least 1 CPU");
    assert_true(fdt_result.cpu_count <= 64, "FDT detection should return reasonable CPU count");

    moss::kernel::logging::klog::info("FDT Detection: {} CPUs ({}% confidence)", fdt_result.cpu_count,
                                      fdt_result.confidence_score);

    // Test Layer 2: Hardware Register Detection
    HardwareRegisterDetector hw_detector;
    auto hw_result = hw_detector.detect();

    assert_true(hw_result.cpu_count >= 1, "Hardware detection should return at least 1 CPU");

    moss::kernel::logging::klog::info("Hardware Detection: {} CPUs ({}% confidence)", hw_result.cpu_count,
                                      hw_result.confidence_score);

    // Test Layer 3: Safe Fallback Detection
    SafeFallbackDetector fallback_detector;
    auto fallback_result = fallback_detector.detect();

    assert_equal(fallback_result.cpu_count, 1u, "Safe fallback should always return 1 CPU");
    assert_equal(fallback_result.confidence_score, 100u, "Safe fallback should have 100% confidence");
    assert_equal(fallback_result.status, DetectionStatus::Success, "Safe fallback should always succeed");

    moss::kernel::logging::klog::info("Safe Fallback Detection: {} CPU ({}% confidence)", fallback_result.cpu_count,
                                      fallback_result.confidence_score);

    cleanup_cpu_detection();
  }

  void test_main_detector() {
    moss::kernel::logging::klog::info("=== Testing Main CPU Topology Detector ===");

    initialize_cpu_detection();

    // Test the main detection function
    auto result = detect_cpu_topology();

    assert_true(result.cpu_count >= 1, "Main detector should return at least 1 CPU");
    assert_true(result.confidence_score > 0, "Main detector should have some confidence");

    moss::kernel::logging::klog::info("Main Detection Result:");
    moss::kernel::logging::klog::info("  CPUs: {}", result.cpu_count);
    moss::kernel::logging::klog::info("  Source: {}", static_cast<int>(result.source));
    moss::kernel::logging::klog::info("  Confidence: {}%", result.confidence_score);
    moss::kernel::logging::klog::info("  Clusters: {}", result.detected_clusters);

#if defined(MOSS_ARCH_ARM64)
    moss::kernel::logging::klog::info("  ARM64 Big cores: {}", result.big_cores);
    moss::kernel::logging::klog::info("  ARM64 Little cores: {}", result.little_cores);
#endif

    // Test applying results to topology system
    apply_detection_results(result);

    // Verify the results were applied
    assert_equal(get_cpu_count(), result.cpu_count, "CPU count should be applied to topology system");

    cleanup_cpu_detection();
  }

  void test_arm64_specific_features() {
#if defined(MOSS_ARCH_ARM64)
    moss::kernel::logging::klog::info("=== Testing ARM64 Specific Features ===");

    initialize_cpu_detection();

    // Test MPIDR reading
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

    u32 cluster_id = (mpidr >> 8) & 0xFF;
    u32 core_id = mpidr & 0xFF;

    moss::kernel::logging::klog::info("Current CPU MPIDR: 0x{:x}", mpidr);
    moss::kernel::logging::klog::info("  Cluster ID: {}", cluster_id);
    moss::kernel::logging::klog::info("  Core ID: {}", core_id);

    // Test hardware register detection specifically
    HardwareRegisterDetector hw_detector;
    auto hw_result = hw_detector.detect();

    // Should detect reasonable number of CPUs for ARM64
    assert_true(hw_result.cpu_count >= 1, "ARM64 hardware detection should find at least 1 CPU");
    assert_true(hw_result.cpu_count <= 32, "ARM64 hardware detection should be reasonable");

    cleanup_cpu_detection();
#else
    moss::kernel::logging::klog::info("=== ARM64 Specific Tests Skipped (not ARM64) ===");
#endif
  }

  void test_error_handling() {
    moss::kernel::logging::klog::info("=== Testing Error Handling and Validation ===");

    // Test detection without initialization (should use emergency fallback)
    auto emergency_result = detect_cpu_topology();
    assert_equal(emergency_result.cpu_count, 1u, "Emergency fallback should return 1 CPU");
    assert_equal(emergency_result.status, DetectionStatus::Failed, "Emergency fallback should indicate failure");

    // Test with proper initialization
    initialize_cpu_detection();

    auto normal_result = detect_cpu_topology();
    assert_true(normal_result.status == DetectionStatus::Success || normal_result.status == DetectionStatus::Partial,
                "Normal detection should succeed or be partial");

    cleanup_cpu_detection();
  }

  const char *name() const override { return "CpuDetectionTest"; }

  void run() override {
    test_detection_layers();
    test_main_detector();
    test_arm64_specific_features();
    test_error_handling();
  }
};

// Register the test
static CpuDetectionTest cpu_detection_test;