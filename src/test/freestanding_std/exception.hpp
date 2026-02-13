#pragma once

/**
 * @file exception.hpp
 * @brief Exception handling stubs for freestanding environment
 *
 * Provides minimal exception support for testing framework.
 * In actual kernel environment, these might be disabled entirely.
 */

#include "type_traits.hpp"
#include "string.hpp"

namespace std {

    /**
     * @brief Base exception class
     */
    class exception {
    public:
        exception() noexcept = default;
        virtual ~exception() = default;

        virtual const char* what() const noexcept {
            return "std::exception";
        }
    };

    /**
     * @brief Logic error exception
     */
    class logic_error : public exception {
    private:
        string message_;

    public:
        explicit logic_error(const string& msg) : message_(msg) {}
        explicit logic_error(const char* msg) : message_(msg) {}

        const char* what() const noexcept override {
            return message_.c_str();
        }
    };

    /**
     * @brief Runtime error exception
     */
    class runtime_error : public exception {
    private:
        string message_;

    public:
        explicit runtime_error(const string& msg) : message_(msg) {}
        explicit runtime_error(const char* msg) : message_(msg) {}

        const char* what() const noexcept override {
            return message_.c_str();
        }
    };

    /**
     * @brief Invalid argument exception
     */
    class invalid_argument : public logic_error {
    public:
        explicit invalid_argument(const string& msg) : logic_error(msg) {}
        explicit invalid_argument(const char* msg) : logic_error(msg) {}
    };

    /**
     * @brief Out of range exception
     */
    class out_of_range : public logic_error {
    public:
        explicit out_of_range(const string& msg) : logic_error(msg) {}
        explicit out_of_range(const char* msg) : logic_error(msg) {}
    };

    /**
     * @brief Length error exception
     */
    class length_error : public logic_error {
    public:
        explicit length_error(const string& msg) : logic_error(msg) {}
        explicit length_error(const char* msg) : logic_error(msg) {}
    };

    /**
     * @brief Bad allocation exception
     */
    class bad_alloc : public exception {
    public:
        const char* what() const noexcept override {
            return "std::bad_alloc";
        }
    };

} // namespace std