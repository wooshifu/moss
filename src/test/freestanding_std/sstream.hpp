#pragma once

/**
 * @file sstream.hpp
 * @brief String stream implementation for freestanding environment
 *
 * Provides std::ostringstream and std::istringstream using static string buffers.
 * Designed for kernel/freestanding environments with fixed-size buffer allocation.
 */

#include "type_traits.hpp"
#include "string.hpp"
#include "iostream.hpp"

namespace std {

    // ========================================================================
    // Constants
    // ========================================================================

    constexpr int EOF = -1;

    // ========================================================================
    // String stream buffer
    // ========================================================================

    template<usize BufferSize = 1024>
    class basic_stringbuf : public streambuf {
    public:
        using string_type = basic_string<BufferSize>;

    private:
        string_type buffer_;
        usize read_pos_;
        usize write_pos_;

    public:
        basic_stringbuf() : buffer_(), read_pos_(0), write_pos_(0) {}

        explicit basic_stringbuf(const string_type& s) : buffer_(s), read_pos_(0), write_pos_(s.size()) {}

        basic_stringbuf(const basic_stringbuf&) = delete;
        basic_stringbuf& operator=(const basic_stringbuf&) = delete;

        basic_stringbuf(basic_stringbuf&& other) noexcept
            : buffer_(static_cast<string_type&&>(other.buffer_))
            , read_pos_(other.read_pos_)
            , write_pos_(other.write_pos_) {
            other.read_pos_ = 0;
            other.write_pos_ = 0;
        }

        basic_stringbuf& operator=(basic_stringbuf&& other) noexcept {
            if (this != &other) {
                buffer_ = static_cast<string_type&&>(other.buffer_);
                read_pos_ = other.read_pos_;
                write_pos_ = other.write_pos_;
                other.read_pos_ = 0;
                other.write_pos_ = 0;
            }
            return *this;
        }

        // Get the string content
        string_type str() const {
            return buffer_;
        }

        // Set the string content
        void str(const string_type& s) {
            buffer_ = s;
            read_pos_ = 0;
            write_pos_ = s.size();
        }

        // Clear the buffer
        void clear() {
            buffer_.clear();
            read_pos_ = 0;
            write_pos_ = 0;
        }

    protected:
    public:
        // Override for output operations
        int overflow(int c) override {
            if (c == EOF) {
                return EOF;
            }

            // Check if we have space
            if (write_pos_ >= BufferSize) {
                return EOF; // Buffer full
            }

            // Append character
            if (write_pos_ < buffer_.size()) {
                buffer_[write_pos_] = static_cast<char>(c);
            } else {
                buffer_.push_back(static_cast<char>(c));
            }

            ++write_pos_;
            return c;
        }

        // Override for input operations
        int underflow() override {
            if (read_pos_ >= buffer_.size()) {
                return EOF;
            }
            return static_cast<unsigned char>(buffer_[read_pos_]);
        }

        int uflow() override {
            if (read_pos_ >= buffer_.size()) {
                return EOF;
            }
            return static_cast<unsigned char>(buffer_[read_pos_++]);
        }

        // Sync operation
        int sync() override {
            // For string streams, sync doesn't need to do anything
            return 0;
        }
    };

    using stringbuf = basic_stringbuf<1024>;

    // ========================================================================
    // Output string stream
    // ========================================================================

    template<usize BufferSize = 1024>
    class basic_ostringstream : public ostream {
    public:
        using string_type = basic_string<BufferSize>;
        using stringbuf_type = basic_stringbuf<BufferSize>;

    private:
        stringbuf_type buffer_;

        // Custom streambuf that integrates with our string buffer
        class ostringstream_buf : public streambuf {
        private:
            stringbuf_type* sb_;

        public:
            explicit ostringstream_buf(stringbuf_type* sb) : sb_(sb) {}

            int overflow(int c) {
                return sb_ ? sb_->overflow(c) : EOF;
            }

            int sync() {
                return sb_ ? sb_->sync() : 0;
            }
        };

        ostringstream_buf stream_buffer_;

    public:
        basic_ostringstream()
            : ostream(&stream_buffer_)
            , buffer_()
            , stream_buffer_(&buffer_) {}

        explicit basic_ostringstream(const string_type& s)
            : ostream(&stream_buffer_)
            , buffer_(s)
            , stream_buffer_(&buffer_) {}

        basic_ostringstream(const basic_ostringstream&) = delete;
        basic_ostringstream& operator=(const basic_ostringstream&) = delete;

        basic_ostringstream(basic_ostringstream&& other) noexcept
            : ostream(&stream_buffer_)
            , buffer_(static_cast<stringbuf_type&&>(other.buffer_))
            , stream_buffer_(&buffer_) {
            // Note: We can't move the base class state easily in this simple implementation
        }

        basic_ostringstream& operator=(basic_ostringstream&& other) noexcept {
            if (this != &other) {
                buffer_ = static_cast<stringbuf_type&&>(other.buffer_);
                stream_buffer_ = ostringstream_buf(&buffer_);
            }
            return *this;
        }

        ~basic_ostringstream() = default;

        // Get the underlying string buffer
        stringbuf_type* rdbuf() {
            return &buffer_;
        }

        const stringbuf_type* rdbuf() const {
            return &buffer_;
        }

        // Get the string content
        string_type str() const {
            return buffer_.str();
        }

        // Set the string content
        void str(const string_type& s) {
            buffer_.str(s);
        }

        // Clear the content
        void clear() {
            buffer_.clear();
        }

        // Custom put that goes directly to our buffer
        basic_ostringstream& put(char c) {
            buffer_.overflow(c);
            return *this;
        }

        // Custom write that goes directly to our buffer
        basic_ostringstream& write(const char* s, usize count) {
            if (s) {
                for (usize i = 0; i < count; ++i) {
                    buffer_.overflow(s[i]);
                }
            }
            return *this;
        }
    };

    using ostringstream = basic_ostringstream<1024>;

    // ========================================================================
    // Input string stream
    // ========================================================================

    template<usize BufferSize = 1024>
    class basic_istringstream {
    public:
        using string_type = basic_string<BufferSize>;
        using stringbuf_type = basic_stringbuf<BufferSize>;

    private:
        stringbuf_type buffer_;
        usize pos_;
        bool good_;

    public:
        basic_istringstream() : buffer_(), pos_(0), good_(true) {}

        explicit basic_istringstream(const string_type& s)
            : buffer_(s), pos_(0), good_(true) {}

        basic_istringstream(const basic_istringstream&) = delete;
        basic_istringstream& operator=(const basic_istringstream&) = delete;

        basic_istringstream(basic_istringstream&& other) noexcept
            : buffer_(static_cast<stringbuf_type&&>(other.buffer_))
            , pos_(other.pos_)
            , good_(other.good_) {
            other.pos_ = 0;
            other.good_ = true;
        }

        basic_istringstream& operator=(basic_istringstream&& other) noexcept {
            if (this != &other) {
                buffer_ = static_cast<stringbuf_type&&>(other.buffer_);
                pos_ = other.pos_;
                good_ = other.good_;
                other.pos_ = 0;
                other.good_ = true;
            }
            return *this;
        }

        // Stream state
        bool good() const { return good_; }
        bool fail() const { return !good_; }
        bool eof() const { return pos_ >= buffer_.str().size(); }
        explicit operator bool() const { return good(); }

        // Get the underlying string buffer
        stringbuf_type* rdbuf() {
            return &buffer_;
        }

        const stringbuf_type* rdbuf() const {
            return &buffer_;
        }

        // Get the string content
        string_type str() const {
            return buffer_.str();
        }

        // Set the string content
        void str(const string_type& s) {
            buffer_.str(s);
            pos_ = 0;
            good_ = true;
        }

        // Clear the content
        void clear() {
            buffer_.clear();
            pos_ = 0;
            good_ = true;
        }

        // Simple extraction for testing purposes
        template<typename T>
        basic_istringstream& operator>>(T& value) {
            // This is a very simplified implementation
            // In a full implementation, this would parse the string properly
            static_cast<void>(value); // Suppress unused parameter warning
            good_ = false; // Mark as failed for simplicity
            return *this;
        }

        // Get a single character
        int get() {
            if (pos_ >= buffer_.str().size()) {
                good_ = false;
                return EOF;
            }
            return static_cast<unsigned char>(buffer_.str()[pos_++]);
        }

        // Get a line
        basic_istringstream& getline(string_type& line, char delim = '\n') {
            line.clear();

            while (pos_ < buffer_.str().size()) {
                char c = buffer_.str()[pos_++];
                if (c == delim) {
                    break;
                }
                line.push_back(c);
            }

            if (pos_ >= buffer_.str().size() && line.empty()) {
                good_ = false;
            }

            return *this;
        }
    };

    using istringstream = basic_istringstream<1024>;

    // ========================================================================
    // Bidirectional string stream
    // ========================================================================

    template<usize BufferSize = 1024>
    class basic_stringstream : public basic_ostringstream<BufferSize> {
    public:
        using string_type = basic_string<BufferSize>;
        using base_type = basic_ostringstream<BufferSize>;

    private:
        usize read_pos_;

    public:
        basic_stringstream() : base_type(), read_pos_(0) {}

        explicit basic_stringstream(const string_type& s)
            : base_type(s), read_pos_(0) {}

        // Input operations
        int get() {
            string_type current_str = this->str();
            if (read_pos_ >= current_str.size()) {
                return EOF;
            }
            return static_cast<unsigned char>(current_str[read_pos_++]);
        }

        basic_stringstream& getline(string_type& line, char delim = '\n') {
            line.clear();
            string_type current_str = this->str();

            while (read_pos_ < current_str.size()) {
                char c = current_str[read_pos_++];
                if (c == delim) {
                    break;
                }
                line.push_back(c);
            }

            return *this;
        }

        // Reset read position
        void seekg(usize pos) {
            read_pos_ = pos;
        }

        usize tellg() const {
            return read_pos_;
        }
    };

    using stringstream = basic_stringstream<1024>;

    // ========================================================================
    // Utility functions
    // ========================================================================

    // Helper to convert values to strings using ostringstream
    template<typename T>
    string to_string(const T& value) {
        basic_ostringstream<512> oss;  // Use same size as string
        oss << value;
        return oss.str();
    }

} // namespace std
