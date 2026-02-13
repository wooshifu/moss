#pragma once

/**
 * @file iostream.hpp
 * @brief UART-based iostream implementation for freestanding environment
 *
 * Provides std::ostream, std::cout and related I/O functionality using direct
 * UART hardware output. Designed for kernel/freestanding environments.
 */

#include "type_traits.hpp"
#include "string.hpp"
#include "string_view.hpp"

namespace std {

    // ========================================================================
    // Forward declarations for MOSS kernel integration
    // ========================================================================

    namespace detail {
        // Interface to MOSS kernel's UART writer
        // This will be implemented to integrate with the actual kernel UART system
        void uart_write_char(char c);
        void uart_write_string(const char* str);
        void uart_write_string(const char* str, usize length);
        void uart_flush();

        // Simple number to string conversion for freestanding environment
        template<typename T>
        string number_to_string(T value);

        // Specializations for different integer types
        template<>
        string number_to_string<int>(int value) {
            if (value == 0) return string("0");

            string result;
            bool negative = false;
            if (value < 0) {
                negative = true;
                value = -value;
            }

            char buffer[32];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            if (negative) {
                result += '-';
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        template<>
        string number_to_string<unsigned int>(unsigned int value) {
            if (value == 0) return string("0");

            string result;
            char buffer[32];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        template<>
        string number_to_string<long>(long value) {
            if (value == 0) return string("0");

            string result;
            bool negative = false;
            if (value < 0) {
                negative = true;
                value = -value;
            }

            char buffer[64];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            if (negative) {
                result += '-';
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        template<>
        string number_to_string<unsigned long>(unsigned long value) {
            if (value == 0) return string("0");

            string result;
            char buffer[64];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        template<>
        string number_to_string<long long>(long long value) {
            if (value == 0) return string("0");

            string result;
            bool negative = false;
            if (value < 0) {
                negative = true;
                value = -value;
            }

            char buffer[64];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            if (negative) {
                result += '-';
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        template<>
        string number_to_string<unsigned long long>(unsigned long long value) {
            if (value == 0) return string("0");

            string result;
            char buffer[64];
            usize index = 0;

            while (value > 0) {
                buffer[index++] = '0' + (value % 10);
                value /= 10;
            }

            // Reverse the digits
            for (usize i = index; i > 0; --i) {
                result += buffer[i - 1];
            }

            return result;
        }

        // Simple floating point to string (basic implementation)
        string float_to_string(double value, int precision = 6) {
            if (value == 0.0) return string("0");

            string result;
            bool negative = false;
            if (value < 0) {
                negative = true;
                value = -value;
            }

            // Extract integer part
            unsigned long long int_part = static_cast<unsigned long long>(value);
            double frac_part = value - int_part;

            if (negative) {
                result += '-';
            }

            // Convert integer part
            result += number_to_string(int_part);

            // Add decimal point and fractional part
            if (precision > 0) {
                result += '.';
                for (int i = 0; i < precision; ++i) {
                    frac_part *= 10;
                    int digit = static_cast<int>(frac_part);
                    result += ('0' + digit);
                    frac_part -= digit;
                }
            }

            return result;
        }
    }

    // ========================================================================
    // Stream buffer base (simplified for freestanding)
    // ========================================================================

    class streambuf {
    public:
        streambuf() = default;
        virtual ~streambuf() = default;

    protected:
        virtual int overflow(int c) {
            detail::uart_write_char(static_cast<char>(c));
            return c;
        }

        virtual int underflow() {
            return -1;  // Default implementation returns EOF
        }

        virtual int uflow() {
            return -1;  // Default implementation returns EOF
        }

        virtual int sync() {
            detail::uart_flush();
            return 0;
        }
    };

    // ========================================================================
    // Basic ostream implementation
    // ========================================================================

    class ostream {
    private:
        streambuf* rdbuf_;
        bool good_;

    public:
        explicit ostream(streambuf* sb) : rdbuf_(sb), good_(true) {}

        virtual ~ostream() = default;

        // Stream state
        bool good() const { return good_; }
        bool fail() const { return !good_; }
        explicit operator bool() const { return good(); }
        bool operator!() const { return fail(); }

        // Character output
        ostream& put(char c) {
            detail::uart_write_char(c);
            return *this;
        }

        // String output
        ostream& write(const char* s, usize n) {
            if (s) {
                detail::uart_write_string(s, n);
            }
            return *this;
        }

        // Flush
        ostream& flush() {
            detail::uart_flush();
            return *this;
        }

        // ====================================================================
        // Insertion operators (operator<<)
        // ====================================================================

        // Character types
        ostream& operator<<(char c) {
            return put(c);
        }

        ostream& operator<<(signed char c) {
            return put(static_cast<char>(c));
        }

        ostream& operator<<(unsigned char c) {
            return put(static_cast<char>(c));
        }

        // String types
        ostream& operator<<(const char* s) {
            if (s) {
                while (*s) {
                    put(*s++);
                }
            }
            return *this;
        }

        ostream& operator<<(const string& s) {
            return write(s.data(), s.size());
        }

        ostream& operator<<(string_view sv) {
            return write(sv.data(), sv.size());
        }

        // Integer types
        ostream& operator<<(int value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        ostream& operator<<(unsigned int value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        ostream& operator<<(long value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        ostream& operator<<(unsigned long value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        ostream& operator<<(long long value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        ostream& operator<<(unsigned long long value) {
            string str = detail::number_to_string(value);
            return *this << str;
        }

        // Floating point types
        ostream& operator<<(float value) {
            string str = detail::float_to_string(static_cast<double>(value));
            return *this << str;
        }

        ostream& operator<<(double value) {
            string str = detail::float_to_string(value);
            return *this << str;
        }

        ostream& operator<<(long double value) {
            string str = detail::float_to_string(static_cast<double>(value));
            return *this << str;
        }

        // Boolean type
        ostream& operator<<(bool value) {
            return *this << (value ? "true" : "false");
        }

        // Pointer types
        ostream& operator<<(const void* ptr) {
            if (!ptr) {
                return *this << "nullptr";
            }

            // Convert pointer to hex string
            usize addr = reinterpret_cast<usize>(ptr);
            string hex_str = "0x";

            if (addr == 0) {
                hex_str += "0";
            } else {
                char hex_digits[] = "0123456789abcdef";
                char buffer[32];
                usize index = 0;

                while (addr > 0) {
                    buffer[index++] = hex_digits[addr % 16];
                    addr /= 16;
                }

                // Reverse the digits
                for (usize i = index; i > 0; --i) {
                    hex_str += buffer[i - 1];
                }
            }

            return *this << hex_str;
        }

        // Stream manipulators
        ostream& operator<<(ostream& (*func)(ostream&)) {
            return func(*this);
        }
    };

    // ========================================================================
    // Stream manipulators
    // ========================================================================

    inline ostream& endl(ostream& os) {
        return os.put('\n').flush();
    }

    inline ostream& flush(ostream& os) {
        return os.flush();
    }

    inline ostream& ends(ostream& os) {
        return os.put('\0');
    }

    // ========================================================================
    // Global stream objects
    // ========================================================================

    namespace detail {
        // Global stream buffer for cout
        class cout_streambuf : public streambuf {
        public:
            cout_streambuf() = default;
            ~cout_streambuf() override = default;

        protected:
            int overflow(int c) override {
                uart_write_char(static_cast<char>(c));
                return c;
            }

            int sync() override {
                uart_flush();
                return 0;
            }
        };

        // Global instances
        inline cout_streambuf& get_cout_buf() {
            static cout_streambuf buf;
            return buf;
        }

        inline ostream& get_cout() {
            static ostream cout_stream(&get_cout_buf());
            return cout_stream;
        }
    }

    // Standard output stream (equivalent to std::cout)
    #define cout (::std::detail::get_cout())

    // ========================================================================
    // UART integration stubs (to be implemented with actual kernel UART)
    // ========================================================================

    namespace detail {
        // These functions need to be implemented to integrate with MOSS kernel UART
        // For now, they are stubs that would be replaced with actual UART calls

        inline void uart_write_char(char c) {
            // TODO: Replace with actual MOSS kernel UART write
            // Example: moss::kernel::uart::write_char(c);

            // Placeholder implementation - in real kernel this would write to UART
            static_cast<void>(c); // Suppress unused parameter warning
        }

        inline void uart_write_string(const char* str) {
            if (str) {
                while (*str) {
                    uart_write_char(*str++);
                }
            }
        }

        inline void uart_write_string(const char* str, usize length) {
            if (str) {
                for (usize i = 0; i < length; ++i) {
                    uart_write_char(str[i]);
                }
            }
        }

        inline void uart_flush() {
            // TODO: Replace with actual MOSS kernel UART flush
            // Example: moss::kernel::uart::flush();
        }
    }

} // namespace std
