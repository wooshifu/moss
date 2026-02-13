/**
 * @file test_containers_modern.cpp
 * @brief Modern ut.hpp-based container tests for MOSS kernel
 *
 * This file contains comprehensive tests for MOSS kernel container implementations
 * using the modern ut.hpp framework with freestanding standard library.
 */

#include "moss_ut.hpp"

using namespace moss::test;

namespace moss::test::containers {

// ============================================================================
// std::vector Tests
// ============================================================================

void test_vector_basic_operations() {
    // Use direct test_case construction instead of literal operators
    boost::ut::test_case test("vector_basic_operations", []() {
        std::vector<int> vec;

        expect(vec.empty());
        expect(vec.size() == 0);

        vec.push_back(42);
        expect(vec.size() == 1);
        expect(vec[0] == 42);

        vec.push_back(24);
        expect(vec.size() == 2);
        expect(vec[1] == 24);

        vec.clear();
        expect(vec.empty());
    });
}

void test_vector_capacity() {
    "vector_capacity"_test = []() {
        std::vector<int> vec;

        // Our freestanding vector has compile-time capacity
        expect(vec.capacity() > 0_u);
        expect(vec.capacity() <= 256_u); // MAX_VECTOR_SIZE

        // Fill to capacity
        for (std::usize i = 0; i < vec.capacity() && i < 100; ++i) {
            vec.push_back(static_cast<int>(i));
        }

        expect(vec.size() <= vec.capacity());
        expect(!vec.empty());
    };
}

void test_vector_iterators() {
    "vector_iterators"_test = []() {
        std::vector<int> vec;
        vec.push_back(1);
        vec.push_back(2);
        vec.push_back(3);
        vec.push_back(4);
        vec.push_back(5);

        int sum = 0;
        for (auto it = vec.begin(); it != vec.end(); ++it) {
            sum += *it;
        }
        expect(sum == 15);

        // Range-based for loop
        int product = 1;
        for (const auto& value : vec) {
            product *= value;
        }
        expect(product == 120); // 5!
    };
}

// ============================================================================
// std::string Tests
// ============================================================================

void test_string_basic_operations() {
    "string_basic_operations"_test = []() {
        std::string str;

        expect(str.empty());
        expect(str.size() == 0_u);

        str = "Hello";
        expect(str.size() == 5_u);
        expect(str == "Hello");

        str += " World";
        expect(str.size() == 11_u);
        expect(str == "Hello World");

        str.clear();
        expect(str.empty());
    };
}

void test_string_capacity() {
    "string_capacity"_test = []() {
        std::string str;

        // Our freestanding string has compile-time capacity
        expect(str.capacity() > 0_u);
        expect(str.capacity() <= 512_u); // MAX_STRING_SIZE

        std::string long_str(100, 'A');
        expect(long_str.size() == 100_u);
        expect(long_str[0] == 'A');
        expect(long_str[99] == 'A');
    };
}

void test_string_operations() {
    "string_operations"_test = []() {
        std::string str = "Hello World";

        expect(str.find("World") != std::string::npos);
        expect(str.find("xyz") == std::string::npos);

        expect(str.substr(0, 5) == "Hello");
        expect(str.substr(6) == "World");

        str.replace(6, 5, "C++");
        expect(str == "Hello C++");
    };
}

// ============================================================================
// std::string_view Tests
// ============================================================================

void test_string_view_basic() {
    "string_view_basic"_test = []() {
        const char* cstr = "Test String";
        std::string_view sv(cstr);

        expect(sv.size() == 11_u);
        expect(!sv.empty());
        expect(sv.data() == cstr);

        expect(sv[0] == 'T');
        expect(sv.back() == 'g');
        expect(sv.front() == 'T');
    };
}

void test_string_view_operations() {
    "string_view_operations"_test = []() {
        std::string_view sv = "Hello World Programming";

        expect(sv.find("World") == 6_u);
        expect(sv.find("xyz") == std::string_view::npos);

        auto sub = sv.substr(0, 5);
        expect(sub == "Hello");

        auto sub2 = sv.substr(6, 5);
        expect(sub2 == "World");
    };
}

// ============================================================================
// std::unordered_map Tests
// ============================================================================

void test_unordered_map_basic() {
    "unordered_map_basic"_test = []() {
        std::unordered_map<int, std::string> map;

        expect(map.empty());
        expect(map.size() == 0_u);

        map[42] = "answer";
        expect(map.size() == 1_u);
        expect(map[42] == "answer");

        map[24] = "reverse";
        expect(map.size() == 2_u);
        expect(map[24] == "reverse");

        map.clear();
        expect(map.empty());
    };
}

void test_unordered_map_operations() {
    "unordered_map_operations"_test = []() {
        std::unordered_map<std::string, int> map;

        map["one"] = 1;
        map["two"] = 2;
        map["three"] = 3;

        expect(map.size() == 3_u);

        auto it = map.find("two");
        expect(it != map.end());
        expect(it->second == 2);

        auto not_found = map.find("four");
        expect(not_found == map.end());

        map.erase("two");
        expect(map.size() == 2_u);
        expect(map.find("two") == map.end());
    };
}

// ============================================================================
// Integration Tests
// ============================================================================

void test_container_interoperability() {
    "container_interoperability"_test = []() {
        std::vector<std::string> vec;
        vec.push_back("Hello");
        vec.push_back("World");
        vec.push_back("C++26");

        std::unordered_map<std::string, std::usize> word_lengths;
        for (const auto& word : vec) {
            word_lengths[word] = word.length();
        }

        expect(word_lengths["Hello"] == 5_u);
        expect(word_lengths["World"] == 5_u);
        expect(word_lengths["C++26"] == 5_u);

        // Test string_view with vector
        std::vector<std::string_view> views;
        for (const auto& word : vec) {
            views.push_back(std::string_view(word));
        }

        expect(views.size() == 3_u);
        expect(views[0] == "Hello");
        expect(views[2] == "C++26");
    };
}

void test_performance_baseline() {
    "performance_baseline"_test = []() {
        using namespace std::chrono;

        auto start = high_resolution_clock::now();

        // Simulate some container operations
        std::vector<int> vec;
        for (int i = 0; i < 1000; ++i) {
            vec.push_back(i);
        }

        std::unordered_map<int, int> map;
        for (int i = 0; i < 100; ++i) {
            map[i] = i * i;
        }

        auto end = high_resolution_clock::now();
        auto duration = duration_cast<std::chrono::microseconds>(end - start);

        // Just verify operations completed (performance test)
        expect(vec.size() == 1000_u);
        expect(map.size() == 100_u);
        expect(duration.count() > 0); // Time measurement works
    };
}

} // namespace moss::test::containers

// ============================================================================
// Test Registration
// ============================================================================

namespace moss::test {

void register_container_tests() {
    containers::test_vector_basic_operations();
    containers::test_vector_capacity();
    containers::test_vector_iterators();

    containers::test_string_basic_operations();
    containers::test_string_capacity();
    containers::test_string_operations();

    containers::test_string_view_basic();
    containers::test_string_view_operations();

    containers::test_unordered_map_basic();
    containers::test_unordered_map_operations();

    containers::test_container_interoperability();
    containers::test_performance_baseline();
}

// Auto-register tests
[[maybe_unused]] static bool container_tests_registered = []() {
    register_container_tests();
    return true;
}();

} // namespace moss::test