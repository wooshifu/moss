// Test Result module compliance with specification requirements
#include <iostream>

import moss.result;

using moss::kernel::Result;
using moss::kernel::ErrorCode;

int main() {
    std::cout << "Testing moss.result module compliance...\n";

    // Test 1: Ok() helper function (no value)
    auto result1 = Ok();
    std::cout << "Ok() creates void result: " << (result1.has_value() ? "✅" : "❌") << "\n";

    // Test 2: Ok(value) helper function
    auto result2 = Ok(42);
    std::cout << "Ok(42) creates int result: " << (result2.has_value() && *result2 == 42 ? "✅" : "❌") << "\n";

    // Test 3: Err(error) helper function
    auto result3 = Err(ErrorCode::OutOfMemory);
    std::cout << "Err(ErrorCode) creates error result: " << (result3.is_err() ? "✅" : "❌") << "\n";

    // Test 4: Union storage structure (basic check)
    Result<int, ErrorCode> result4 = Ok(100);
    std::cout << "Union storage working: " << (result4.has_value() && *result4 == 100 ? "✅" : "❌") << "\n";

    // Test 5: Result<void, E> specialization
    Result<void, ErrorCode> result5 = Ok();
    Result<void, ErrorCode> result6 = Err(ErrorCode::InvalidParameter);
    std::cout << "void specialization working: " << (result5.is_ok() && result6.is_err() ? "✅" : "❌") << "\n";

    // Test 6: Required methods
    bool methods_work = result2.has_value() && result2.is_ok() && !result2.is_err();
    std::cout << "Required methods working: " << (methods_work ? "✅" : "❌") << "\n";

    std::cout << "\nAll specification requirements: ";
    if (result1.has_value() && result2.has_value() && result3.is_err() &&
        result4.has_value() && result5.is_ok() && result6.is_err() && methods_work) {
        std::cout << "✅ SPEC COMPLIANT\n";
        return 0;
    } else {
        std::cout << "❌ STILL HAS ISSUES\n";
        return 1;
    }
}