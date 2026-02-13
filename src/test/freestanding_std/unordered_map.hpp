#pragma once

/**
 * @file unordered_map.hpp
 * @brief Static std::unordered_map replacement for freestanding environment
 *
 * Provides std::unordered_map API with fixed-size bucket array using chained hashing.
 * Uses static allocation instead of dynamic allocation, making it suitable for
 * kernel/freestanding use.
 */

#include "type_traits.hpp"

namespace std {

    // ========================================================================
    // Basic pair implementation (needed for unordered_map)
    // ========================================================================

    template<typename T1, typename T2>
    struct pair {
        using first_type = T1;
        using second_type = T2;

        T1 first;
        T2 second;

        constexpr pair() : first(), second() {}

        constexpr pair(const T1& x, const T2& y) : first(x), second(y) {}

        template<typename U1, typename U2>
        constexpr pair(U1&& x, U2&& y) : first(static_cast<U1&&>(x)), second(static_cast<U2&&>(y)) {}

        constexpr pair(const pair&) = default;
        constexpr pair(pair&&) = default;

        pair& operator=(const pair&) = default;
        pair& operator=(pair&&) = default;

        template<typename U1, typename U2>
        pair& operator=(const pair<U1, U2>& other) {
            first = other.first;
            second = other.second;
            return *this;
        }

        template<typename U1, typename U2>
        pair& operator=(pair<U1, U2>&& other) {
            first = static_cast<U1&&>(other.first);
            second = static_cast<U2&&>(other.second);
            return *this;
        }
    };

    // Helper function to create pairs (like std::make_pair)
    template<typename T1, typename T2>
    constexpr pair<T1, T2> make_pair(T1&& t, T2&& u) {
        return pair<T1, T2>(static_cast<T1&&>(t), static_cast<T2&&>(u));
    }

    // Comparison operators for pair
    template<typename T1, typename T2>
    constexpr bool operator==(const pair<T1, T2>& lhs, const pair<T1, T2>& rhs) {
        return lhs.first == rhs.first && lhs.second == rhs.second;
    }

    template<typename T1, typename T2>
    constexpr bool operator!=(const pair<T1, T2>& lhs, const pair<T1, T2>& rhs) {
        return !(lhs == rhs);
    }

    template<typename T1, typename T2>
    constexpr bool operator<(const pair<T1, T2>& lhs, const pair<T1, T2>& rhs) {
        return lhs.first < rhs.first || (!(rhs.first < lhs.first) && lhs.second < rhs.second);
    }

    /**
     * @brief Static unordered_map implementation with compile-time bucket size
     *
     * Provides complete std::unordered_map API using static storage allocation.
     * Uses chained hashing with linked list for collision resolution.
     *
     * @tparam K Key type
     * @tparam V Value type
     * @tparam Buckets Number of buckets (default: 128)
     * @tparam MaxEntries Maximum number of key-value entries (default: 256)
     */
    template<typename K, typename V, usize Buckets = 128, usize MaxEntries = 256>
    class unordered_map {
    public:
        // ====================================================================
        // Type definitions (std::unordered_map compatibility)
        // ====================================================================

        using key_type = K;
        using mapped_type = V;
        using value_type = pair<const K, V>;
        using size_type = usize;
        using difference_type = isize;
        using hasher = hash<K>;
        using reference = value_type&;
        using const_reference = const value_type&;
        using pointer = value_type*;
        using const_pointer = const value_type*;

    private:
        // ====================================================================
        // Internal node structure for chained hashing
        // ====================================================================

        struct Node {
            value_type data;
            Node* next;
            bool in_use;

            template<typename... Args>
            Node(Args&&... args) : data(static_cast<Args&&>(args)...), next(nullptr), in_use(true) {}

            Node() : data(), next(nullptr), in_use(false) {}
        };

        // Static storage for nodes and buckets
        alignas(Node) u8 node_storage_[MaxEntries * sizeof(Node)];
        Node* buckets_[Buckets];  // Array of head pointers for each bucket
        size_type size_;
        size_type next_free_node_;

        Node* node_ptr(size_type index) noexcept {
            return reinterpret_cast<Node*>(node_storage_ + index * sizeof(Node));
        }

        const Node* node_ptr(size_type index) const noexcept {
            return reinterpret_cast<const Node*>(node_storage_ + index * sizeof(Node));
        }

    public:
        // ====================================================================
        // Simple iterator implementation
        // ====================================================================

        class iterator {
        private:
            unordered_map* map_;
            size_type bucket_;
            Node* node_;

            void advance_to_next() {
                if (node_ && node_->next) {
                    node_ = node_->next;
                    return;
                }

                // Move to next bucket with data
                ++bucket_;
                node_ = nullptr;
                while (bucket_ < Buckets && !node_) {
                    node_ = map_->buckets_[bucket_];
                    if (!node_) ++bucket_;
                }
            }

        public:
            iterator(unordered_map* map, size_type bucket, Node* node)
                : map_(map), bucket_(bucket), node_(node) {}

            iterator() : map_(nullptr), bucket_(0), node_(nullptr) {}

            reference operator*() noexcept {
                return node_->data;
            }

            pointer operator->() noexcept {
                return &node_->data;
            }

            iterator& operator++() noexcept {
                advance_to_next();
                return *this;
            }

            iterator operator++(int) noexcept {
                iterator tmp = *this;
                advance_to_next();
                return tmp;
            }

            bool operator==(const iterator& other) const noexcept {
                return map_ == other.map_ && bucket_ == other.bucket_ && node_ == other.node_;
            }

            bool operator!=(const iterator& other) const noexcept {
                return !(*this == other);
            }
        };

        class const_iterator {
        private:
            const unordered_map* map_;
            size_type bucket_;
            const Node* node_;

            void advance_to_next() {
                if (node_ && node_->next) {
                    node_ = node_->next;
                    return;
                }

                ++bucket_;
                node_ = nullptr;
                while (bucket_ < Buckets && !node_) {
                    node_ = map_->buckets_[bucket_];
                    if (!node_) ++bucket_;
                }
            }

        public:
            const_iterator(const unordered_map* map, size_type bucket, const Node* node)
                : map_(map), bucket_(bucket), node_(node) {}

            const_iterator() : map_(nullptr), bucket_(0), node_(nullptr) {}

            const_iterator(const iterator& it)
                : map_(it.map_), bucket_(it.bucket_), node_(it.node_) {}

            const_reference operator*() const noexcept {
                return node_->data;
            }

            const_pointer operator->() const noexcept {
                return &node_->data;
            }

            const_iterator& operator++() noexcept {
                advance_to_next();
                return *this;
            }

            const_iterator operator++(int) noexcept {
                const_iterator tmp = *this;
                advance_to_next();
                return tmp;
            }

            bool operator==(const const_iterator& other) const noexcept {
                return map_ == other.map_ && bucket_ == other.bucket_ && node_ == other.node_;
            }

            bool operator!=(const const_iterator& other) const noexcept {
                return !(*this == other);
            }
        };

        // ====================================================================
        // Constructors and Destructor
        // ====================================================================

        /**
         * @brief Default constructor - creates empty unordered_map
         */
        unordered_map() : size_(0), next_free_node_(0) {
            // Initialize bucket array to nullptr
            for (size_type i = 0; i < Buckets; ++i) {
                buckets_[i] = nullptr;
            }

            // Initialize node storage
            for (size_type i = 0; i < MaxEntries; ++i) {
                node_ptr(i)->in_use = false;
                node_ptr(i)->next = nullptr;
            }
        }

        /**
         * @brief Copy constructor
         */
        unordered_map(const unordered_map& other) : unordered_map() {
            for (const auto& pair : other) {
                insert(pair);
            }
        }

        /**
         * @brief Move constructor
         */
        unordered_map(unordered_map&& other) noexcept : unordered_map() {
            *this = static_cast<unordered_map&&>(other);
        }

        /**
         * @brief Destructor
         */
        ~unordered_map() {
            clear();
        }

        // ====================================================================
        // Assignment operators
        // ====================================================================

        unordered_map& operator=(const unordered_map& other) {
            if (this != &other) {
                clear();
                for (const auto& pair : other) {
                    insert(pair);
                }
            }
            return *this;
        }

        unordered_map& operator=(unordered_map&& other) noexcept {
            if (this != &other) {
                clear();

                // Move bucket pointers and data
                for (size_type i = 0; i < Buckets; ++i) {
                    buckets_[i] = other.buckets_[i];
                    other.buckets_[i] = nullptr;
                }

                // Move node storage
                for (size_type i = 0; i < MaxEntries; ++i) {
                    if (other.node_ptr(i)->in_use) {
                        new(node_ptr(i)) Node(static_cast<Node&&>(*other.node_ptr(i)));
                        other.node_ptr(i)->in_use = false;
                    }
                }

                size_ = other.size_;
                next_free_node_ = other.next_free_node_;
                other.size_ = 0;
                other.next_free_node_ = 0;
            }
            return *this;
        }

        // ====================================================================
        // Element access
        // ====================================================================

        mapped_type& at(const key_type& key) {
            Node* node = find_node(key);
            if (!node) {
                // In kernel environment: can't throw, return reference to default value
                // This is unsafe but better than crashing
                static mapped_type default_value{};
                return default_value;
            }
            return node->data.second;
        }

        const mapped_type& at(const key_type& key) const {
            const Node* node = find_node(key);
            if (!node) {
                static const mapped_type default_value{};
                return default_value;
            }
            return node->data.second;
        }

        mapped_type& operator[](const key_type& key) {
            Node* node = find_node(key);
            if (!node) {
                // Insert with default-constructed value
                auto result = insert(make_pair(key, mapped_type{}));
                return result.first->second;
            }
            return node->data.second;
        }

        // ====================================================================
        // Iterators
        // ====================================================================

        iterator begin() noexcept {
            // Find first bucket with data
            for (size_type i = 0; i < Buckets; ++i) {
                if (buckets_[i]) {
                    return iterator(this, i, buckets_[i]);
                }
            }
            return end();
        }

        const_iterator begin() const noexcept {
            for (size_type i = 0; i < Buckets; ++i) {
                if (buckets_[i]) {
                    return const_iterator(this, i, buckets_[i]);
                }
            }
            return end();
        }

        const_iterator cbegin() const noexcept {
            return begin();
        }

        iterator end() noexcept {
            return iterator(this, Buckets, nullptr);
        }

        const_iterator end() const noexcept {
            return const_iterator(this, Buckets, nullptr);
        }

        const_iterator cend() const noexcept {
            return end();
        }

        // ====================================================================
        // Capacity
        // ====================================================================

        [[nodiscard]] bool empty() const noexcept {
            return size_ == 0;
        }

        size_type size() const noexcept {
            return size_;
        }

        constexpr size_type max_size() const noexcept {
            return MaxEntries;
        }

        // ====================================================================
        // Modifiers
        // ====================================================================

        void clear() noexcept {
            // Destroy all nodes
            for (size_type i = 0; i < MaxEntries; ++i) {
                if (node_ptr(i)->in_use) {
                    node_ptr(i)->~Node();
                    node_ptr(i)->in_use = false;
                }
            }

            // Clear bucket pointers
            for (size_type i = 0; i < Buckets; ++i) {
                buckets_[i] = nullptr;
            }

            size_ = 0;
            next_free_node_ = 0;
        }

        pair<iterator, bool> insert(const value_type& value) {
            return emplace(value.first, value.second);
        }

        pair<iterator, bool> insert(value_type&& value) {
            return emplace(static_cast<const K&>(value.first), static_cast<V&&>(value.second));
        }

        template<typename... Args>
        pair<iterator, bool> emplace(const K& key, Args&&... args) {
            // Check if key already exists
            Node* existing = find_node(key);
            if (existing) {
                return make_pair(iterator(this, hash_key(key), existing), false);
            }

            // Check if we have space
            if (size_ >= MaxEntries) {
                return make_pair(end(), false);
            }

            // Find free node
            Node* new_node = allocate_node();
            if (!new_node) {
                return make_pair(end(), false);
            }

            // Construct the pair in place
            new(new_node) Node(make_pair(key, mapped_type(static_cast<Args&&>(args)...)));

            // Insert into bucket
            size_type bucket_index = hash_key(key);
            new_node->next = buckets_[bucket_index];
            buckets_[bucket_index] = new_node;

            ++size_;
            return make_pair(iterator(this, bucket_index, new_node), true);
        }

        size_type erase(const key_type& key) {
            size_type bucket_index = hash_key(key);
            Node** current = &buckets_[bucket_index];

            while (*current) {
                if ((*current)->data.first == key) {
                    Node* to_delete = *current;
                    *current = to_delete->next;
                    deallocate_node(to_delete);
                    --size_;
                    return 1;
                }
                current = &(*current)->next;
            }
            return 0;
        }

        iterator erase(const_iterator pos) {
            if (pos == end()) {
                return end();
            }

            size_type count = erase(pos->first);
            if (count == 0) {
                return end();
            }

            // Return iterator to next element
            iterator result(this, pos.bucket_, pos.node_);
            ++result;
            return result;
        }

        // ====================================================================
        // Lookup
        // ====================================================================

        size_type count(const key_type& key) const {
            return find_node(key) ? 1 : 0;
        }

        iterator find(const key_type& key) {
            Node* node = find_node(key);
            if (node) {
                return iterator(this, hash_key(key), node);
            }
            return end();
        }

        const_iterator find(const key_type& key) const {
            const Node* node = find_node(key);
            if (node) {
                return const_iterator(this, hash_key(key), node);
            }
            return end();
        }

    private:
        // ====================================================================
        // Helper functions
        // ====================================================================

        size_type hash_key(const key_type& key) const {
            hasher hash_fn;
            return hash_fn(key) % Buckets;
        }

        Node* find_node(const key_type& key) {
            size_type bucket_index = hash_key(key);
            Node* current = buckets_[bucket_index];

            while (current) {
                if (current->data.first == key) {
                    return current;
                }
                current = current->next;
            }
            return nullptr;
        }

        const Node* find_node(const key_type& key) const {
            size_type bucket_index = hash_key(key);
            const Node* current = buckets_[bucket_index];

            while (current) {
                if (current->data.first == key) {
                    return current;
                }
                current = current->next;
            }
            return nullptr;
        }

        Node* allocate_node() {
            // Linear search for free node (could be optimized with free list)
            for (size_type i = next_free_node_; i < MaxEntries; ++i) {
                if (!node_ptr(i)->in_use) {
                    next_free_node_ = i + 1;
                    node_ptr(i)->in_use = true;
                    return node_ptr(i);
                }
            }

            // Wrap around and search from beginning
            for (size_type i = 0; i < next_free_node_; ++i) {
                if (!node_ptr(i)->in_use) {
                    next_free_node_ = i + 1;
                    node_ptr(i)->in_use = true;
                    return node_ptr(i);
                }
            }

            return nullptr; // No free nodes
        }

        void deallocate_node(Node* node) {
            if (node >= node_ptr(0) && node < node_ptr(MaxEntries)) {
                node->~Node();
                node->in_use = false;
                node->next = nullptr;

                // Update next_free_node_ hint
                size_type index = static_cast<size_type>(node - node_ptr(0));
                if (index < next_free_node_) {
                    next_free_node_ = index;
                }
            }
        }
    };

} // namespace std
