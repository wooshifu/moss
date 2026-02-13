/**
 * @file boost_ut_kernel_validation.cpp
 * @brief Validate boost::ut kernel features work correctly in freestanding env
 */

#include "../framework/ut_kernel.hpp"
#include "../framework/moss_ut.hpp"

using namespace boost::ut;

// Test integer and boolean literals
static auto test_literals = "literals"_test = [] {
    // Integer literals
    int i_val = 42_i;
    expect(i_val == 42);

    // Boolean literals
    bool b_true = true_b;
    bool b_false = false_b;
    expect(b_true == true);
    expect(b_false == false);

    // Character literals
    char c_val = 'K'_c;
    expect(c_val == 'K');
};

// Test BDD syntax (given/when/then)
static auto test_bdd = "bdd_syntax"_test = [] {
    given("memory manager is initialized") = [] {
        bool initialized = true;
        expect(initialized);
    };

    when("allocating kernel memory") = [] {
        int allocated_size = 1024;
        expect(allocated_size > 0);
    };

    then("memory should be properly aligned") = [] {
        unsigned long addr = 0x1000;
        expect((addr & 0xFFF) == 0);  // 4K aligned
    };
};

// Test kernel error handling utilities
static auto test_error_handling = "error_handling"_test = [] {
    expect_no_error(static_cast<int>(kernel_error_code::success));

    expect_error_code(static_cast<int>(kernel_error_code::invalid_argument),
                     kernel_error_code::invalid_argument);
    expect_error_code(static_cast<int>(kernel_error_code::out_of_memory),
                     kernel_error_code::out_of_memory);

    expect_kernel_panic([]{ test_kernel_panic(); });
};

// Test parameterized containers
static auto test_param_containers = "param_containers"_test = [] {
    auto params1 = MAKE_TEST_PARAMS_1(int, 100);
    auto params2 = MAKE_TEST_PARAMS_2(char, 'A', 'B');
    auto params4 = MAKE_TEST_PARAMS_4(bool, true, false, true, false);

    expect(params1.size() == 1);
    expect(params1[0] == 100);

    expect(params2.size() == 2);
    expect(params2[0] == 'A');
    expect(params2[1] == 'B');

    expect(params4.size() == 4);
    expect(params4[0] == true);
    expect(params4[1] == false);
};

// Test composite expect conditions
static auto test_complex_conditions = "complex_conditions"_test = [] {
    expect(42 > 40 and 42 < 50);
    expect(true_b or false_b);
    expect(eq(1 + 1, 2));
    expect(ne(1, 2));
    expect(gt(10, 5));
    expect(lt(5, 10));
};

// Force registration in freestanding environment
extern "C" void force_boost_ut_kernel_validation_registration() {
    // Static initializers above handle registration
}
