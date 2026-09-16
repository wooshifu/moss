"""Run the production intrusive/pool/MPMC queue methods on host threads."""

import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="module")
def queue_binary(tmp_path_factory):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("clang++ required for production queue regression checks")
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/containers/src/containers.cppm").read_text()
    # Keep the actual queue and blocking lock bodies. Only atomics, CPU yield,
    # and interrupt state use host equivalents; no queue algorithm is mirrored.
    locks = source[source.index("inline void (*g_preempt_disable_fn)") : source.index("// Per-CPU counter")]
    queues = source[source.index("// Queue node base") : source.index("// Fixed queue budgets")]
    support = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
namespace moss {
using std::move; using std::forward; using std::is_nothrow_constructible_v;
using u8=std::uint8_t;
namespace kernel {
using usize=std::size_t; using u32=std::uint32_t;
constexpr usize CACHE_LINE_SIZE=64, PAGE_SIZE=4096;
namespace arch {
inline thread_local bool irq_enabled=true;
inline bool interrupts_enabled(){return irq_enabled;}
inline void disable_interrupts(){irq_enabled=false;}
inline void enable_interrupts(){irq_enabled=true;}
inline void cpu_yield(){
  // Match the kernel's polling hint rather than asking the host scheduler to
  // deschedule every ticket waiter, which makes loaded-host runs unbounded.
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}
inline unsigned get_current_cpu_id(){return 0;}
}
namespace containers {
enum class MemoryOrder {Relaxed,Acquire,Release,AcqRel,SeqCst};
inline std::memory_order order(MemoryOrder o){
  switch(o){case MemoryOrder::Relaxed:return std::memory_order_relaxed;
  case MemoryOrder::Acquire:return std::memory_order_acquire;
  case MemoryOrder::Release:return std::memory_order_release;
  case MemoryOrder::AcqRel:return std::memory_order_acq_rel;
  default:return std::memory_order_seq_cst;}}
template<class T> struct AtomicCounter {
  std::atomic<T> value;
  constexpr AtomicCounter(T v=0):value(v){}
  T load(MemoryOrder o=MemoryOrder::SeqCst)const{return value.load(order(o));}
  void store(T v,MemoryOrder o=MemoryOrder::SeqCst){value.store(v,order(o));}
  T fetch_add(T v,MemoryOrder o=MemoryOrder::SeqCst){return value.fetch_add(v,order(o));}
  bool compare_exchange_weak(T &e,T d,MemoryOrder o,MemoryOrder f){
    return value.compare_exchange_weak(e,d,order(o),order(f));}
};
using AtomicU32=AtomicCounter<moss::kernel::u32>;
template<class T>struct alignas(CACHE_LINE_SIZE) CacheAlignedAtomic:AtomicCounter<T>{
  using AtomicCounter<T>::AtomicCounter;
};
template<class T>struct AtomicPtr {
  std::atomic<T*> value;
  constexpr AtomicPtr(T *v=nullptr):value(v){}
  T *load(MemoryOrder o=MemoryOrder::SeqCst)const{return value.load(order(o));}
  void store(T *v,MemoryOrder o=MemoryOrder::SeqCst){value.store(v,order(o));}
  T *exchange(T *v,MemoryOrder o=MemoryOrder::SeqCst){return value.exchange(v,order(o));}
};
"""
    cases = r"""
using namespace moss::kernel::containers;
static thread_local unsigned preempt_depth=0;
static void disable_preempt()noexcept{++preempt_depth;}
static void enable_preempt()noexcept{assert(preempt_depth);--preempt_depth;}
static void balanced(){assert(preempt_depth==0 && moss::kernel::arch::irq_enabled);}
static std::atomic<unsigned> assignments{0};
static std::atomic<bool> hold_first{false}, release_first{false};
struct Item {
  std::atomic<unsigned> value{0};
  Item()=default;
  explicit Item(unsigned v):value(v){}
  Item &operator=(Item &&other)noexcept{
    unsigned ordinal=assignments.fetch_add(1);
    if(hold_first && ordinal==0)
      while(!release_first.load())std::this_thread::yield();
    value.store(other.value.load());return *this;
  }
};
static void wait_for_first(){
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(assignments.load()==0){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
static void permit_overlap(std::atomic<bool> &started){
  while(!started.load())std::this_thread::yield();
  // The first operation is already inside the real SPSC value move. Give the
  // second runnable operation 100 ms to expose an unprotected same-lane move;
  // the assertion below checks lost/duplicated data, not elapsed performance.
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
  while(assignments.load()<2 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  release_first=true;
}
int main(int argc,char **argv){
  assert(argc==2);g_preempt_disable_fn=disable_preempt;g_preempt_enable_fn=enable_preempt;
  const int which=std::atoi(argv[1]);
  if(which==0){
    MPSCQueue<unsigned> queue;QueueNode<unsigned> a(1),b(2);
    queue.enqueue(&a);queue.enqueue(&b);assert(queue.try_dequeue()==&a);
    queue.enqueue(&a); // Immediate recycling must not unlink the unread b.
    assert(queue.try_dequeue()==&b);assert(queue.try_dequeue()==&a);
    assert(queue.try_dequeue()==nullptr && queue.empty());balanced();return 0;
  }
  if(which==1){
    MPSCQueue<unsigned> queue;auto *a=new QueueNode<unsigned>(1);auto *b=new QueueNode<unsigned>(2);
    queue.enqueue(a);queue.enqueue(b);assert(queue.try_dequeue()==a);delete a;
    assert(queue.try_dequeue()==b);delete b;
    assert(queue.try_dequeue()==nullptr && queue.empty());balanced();return 0;
  }
  if(which==2){
    ObjectPool<unsigned,4> pool;auto *first=pool.allocate();assert(first);first->data=99;
    pool.deallocate(first); // Resetting the current dummy used to lose three nodes.
    QueueNode<unsigned> *nodes[4]{};
    for(unsigned i=0;i<4;++i){nodes[i]=pool.allocate();assert(nodes[i]);
      assert(nodes[i]->data==0);for(unsigned j=0;j<i;++j)assert(nodes[i]!=nodes[j]);}
    assert(!pool.allocate() && !pool.has_available());
    for(auto *node:nodes)pool.deallocate(node);assert(pool.has_available());balanced();return 0;
  }
  if(which==3 || which==4){
    MPMCQueue<Item,1,4> queue;Item seed(7);bool first=false,second=false;
    if(which==4)assert(queue.try_enqueue(std::move(seed)));
    assignments=0;release_first=false;hold_first=true;
    Item a(1),b(2),out_a,out_b;std::atomic<bool> second_started{false};
    std::thread one([&]{first=which==3?queue.try_enqueue(std::move(a)):queue.try_dequeue(0,out_a);balanced();});
    wait_for_first();
    std::thread two([&]{second_started=true;
      second=which==3?queue.try_enqueue(std::move(b)):queue.try_dequeue_any(out_b);balanced();});
    permit_overlap(second_started);one.join();two.join();hold_first=false;
    if(which==3){
      assert(first && second);assert(queue.try_dequeue(0,out_a));assert(queue.try_dequeue_any(out_b));
      assert(out_a.value==1 && out_b.value==2);assert(queue.empty());
    }else{assert(first && !second && out_a.value==7 && queue.empty());}
    balanced();return 0;
  }
  if(which==6){
    // Eight users contend for four nodes, exercising simultaneous allocation
    // and return while checking unique leases and reset payloads every time.
    constexpr unsigned pool_size=4,workers=8,iterations=1000;
    ObjectPool<unsigned,pool_size> pool;QueueNode<unsigned> *nodes[pool_size];
    std::atomic<unsigned> leases[pool_size]{};std::thread threads[workers];
    for(auto &node:nodes){node=pool.allocate();assert(node);}
    for(auto *node:nodes)pool.deallocate(node);
    for(auto &thread:threads)thread=std::thread([&]{
      for(unsigned i=0;i<iterations;++i){QueueNode<unsigned> *node;
        while(!(node=pool.allocate()))std::this_thread::yield();
        unsigned index=0;while(index<pool_size && nodes[index]!=node)++index;
        assert(index<pool_size && leases[index].fetch_add(1)==0);
        assert(node->data==0);node->data=i+1;
        assert(leases[index].fetch_sub(1)==1);pool.deallocate(node);
      }balanced();});
    for(auto &thread:threads)thread.join();
    for(auto &node:nodes){node=pool.allocate();assert(node);}
    assert(!pool.allocate());for(auto *node:nodes)pool.deallocate(node);balanced();return 0;
  }
  if(which==7){
    // Four producers publish 2000 distinct nodes each to a single consumer.
    constexpr unsigned workers=4,n=2000,total=workers*n;
    MPSCQueue<unsigned> queue;auto *nodes=new QueueNode<unsigned>[total];
    auto *seen=new bool[total]{};std::thread writers[workers];
    for(unsigned id=0;id<total;++id)nodes[id].data=id;
    for(unsigned worker=0;worker<workers;++worker)writers[worker]=std::thread([&,worker]{
      for(unsigned i=0;i<n;++i)queue.enqueue(&nodes[worker*n+i]);balanced();});
    unsigned consumed=0;while(consumed<total){if(auto *node=queue.try_dequeue()){
      assert(node->data<total && !seen[node->data]);seen[node->data]=true;++consumed;
    }else std::this_thread::yield();}
    for(auto &thread:writers)thread.join();assert(queue.empty());
    delete[] seen;delete[] nodes;balanced();return 0;
  }
  if(which==8){moss::test::queue_regression::run();balanced();return 0;}
  // Four independent producers/consumers and tiny lanes force wrap and steal.
  // 2000 IDs per producer give repeated pressure without a benchmark claim.
  constexpr unsigned workers=4,n=2000,total=workers*n;
  MPMCQueue<unsigned,2,8> queue;std::atomic<unsigned> consumed{0};
  auto *seen=new std::atomic<unsigned>[total]{};
  std::thread writers[workers],readers[workers];
  for(unsigned worker=0;worker<workers;++worker){
    writers[worker]=std::thread([&,worker]{for(unsigned i=0;i<n;++i){unsigned id=worker*n+i;
      while(!queue.try_enqueue(id))std::this_thread::yield();}balanced();});
    readers[worker]=std::thread([&]{unsigned id;
      while(consumed.load()<total){if(queue.try_dequeue_any(id)){
        assert(id<total && seen[id].fetch_add(1)==0);consumed.fetch_add(1);
      }else std::this_thread::yield();}balanced();});
  }
  for(auto &thread:writers)thread.join();for(auto &thread:readers)thread.join();
  for(unsigned id=0;id<total;++id)assert(seen[id]==1);
  delete[] seen;assert(queue.empty() && queue.approximate_total_size()==0);balanced();
}
"""
    directory = tmp_path_factory.mktemp("container-queues-production")
    cpp = directory / "queues.cpp"
    binary = directory / "queues"
    helper = (root / "src/test/queue_regression.hpp").read_text()
    # Execute the unchanged QEMU helper here as well; host assertion reporting
    # replaces only the validation framework, preserving every helper condition.
    assertions = "\n}}}\nnamespace boost::ut {inline bool expect(bool value){assert(value);return value;}}\n"
    cpp.write_text(support + locks + queues + assertions + helper + cases)
    compiled = subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-O1",
            "-pthread",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            str(cpp),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert compiled.returncode == 0, compiled.stderr
    return binary


@pytest.mark.parametrize(
    "case",
    range(9),
    ids=[
        "immediate-node-reuse",
        "returned-node-destruction",
        "pool-recycle-capacity",
        "same-lane-producers",
        "same-lane-consumers-and-steal",
        "mpmc-threaded-exactly-once",
        "pool-concurrent-leases",
        "mpsc-threaded-exactly-once",
        "native-helper",
    ],
)
def test_production_queue_contracts(queue_binary, case):
    # An ASan host run during concurrent preset builds took 40.63 s wall time
    # but only 0.08 s user/0.30 s system CPU. Allow 90 s for host startup and
    # scheduling delay while still bounding hangs; this is no queue/QEMU SLA.
    result = subprocess.run([str(queue_binary), str(case)], capture_output=True, text=True, timeout=90, check=False)
    assert result.returncode == 0, result.stderr
