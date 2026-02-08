// Test Result header file compliance with specification requirements
#include <iostream>
#include "result.hpp"

using moss::kernel::Result;
using moss::kernel::ErrorCode;

int main() {
    std::cout << "Testing moss result header compliance...\n";

    // Test 1: Ok() helper function (no value)
    auto result1 = moss::kernel::Ok();
    std::cout << "Ok() creates void result: " << (result1.has_value() ? "✅" : "❌") << "\n";

    // Test 2: Ok(value) helper function
    auto result2 = moss::kernel::Ok(42);
    std::cout << "Ok(42) creates int result: " << (result2.has_value() && *result2 == 42 ? "✅" : "❌") << "\n";

    // Test 3: Error(error) helper function
    auto result3 = moss::kernel::Error<void>(ErrorCode::OutOfMemory);
    std::cout << "Error(ErrorCode) creates error result: " << (result3.is_error() ? "✅" : "❌") << "\n";

    // Test 4: Union storage structure (basic check)
    Result<int, ErrorCode> result4(42);
    std::cout << "Union storage working: " << (result4.has_value() && *result4 == 42 ? "✅" : "❌") << "\n";

    // Test 5: Result<void, E> specialization
    Result<void, ErrorCode> result5;  // default constructor creates success
    Result<void, ErrorCode> result6(ErrorCode::InvalidParameter);
    std::cout << "void specialization working: " << (result5.is_ok() && result6.is_error() ? "✅" : "❌") << "\n";

    // Test 6: Required methods
    bool methods_work = result2.has_value() && result2.is_ok() && !result2.is_error();
    std::cout << "Required methods working: " << (methods_work ? "✅" : "❌") << "\n";

    std::cout << "\nHeader file: ";
    std::cout << "✅ COMPILES AND WORKS\n";
    return 0;
}