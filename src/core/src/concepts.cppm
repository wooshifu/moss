// MOSS Concepts Module - Type Constraints for Freestanding Environment
// Provides marker types and concepts for containers and memory management
export module moss.concepts;

import moss.std;

// Container concepts (marker types)
export namespace moss::concepts {

template <typename T> struct Container {
  using type = T;
};

template <typename T> struct IterableContainer {
  using type = T;
};

template <typename T> struct SequenceContainer {
  using type = T;
};

template <typename T> struct Stack {
  using type = T;
};

template <typename T> struct Queue {
  using type = T;
};

template <typename T> struct LockFree {
  using type = T;
};

template <typename T> struct MemoryPool {
  using type = T;
};

template <typename T> struct SlabAllocator {
  using type = T;
};

template <typename T> struct RCUDataStructure {
  using type = T;
};

} // namespace moss::concepts

// Memory concepts (marker types)
export namespace moss::concepts {

template <typename T> struct UniquePtrCompatible {
  using type = T;
};

template <typename T> struct SmartPointer {
  using type = T;
};

template <typename Alloc> struct Allocator {
  using type = Alloc;
};

template <typename T, typename... Args> struct Constructible {
  using type = T;
};

template <typename T> struct MoveConstructible {
  using type = T;
};

template <typename T> struct CopyConstructible {
  using type = T;
};

template <typename T> struct Destructible {
  using type = T;
};

template <typename T> struct RAIIResource {
  using type = T;
};

} // namespace moss::concepts
