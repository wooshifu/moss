"""Run the production ring algorithm on host threads; QEMU tests cover kernel locks."""

import shutil
import subprocess
from pathlib import Path

import pytest


@pytest.fixture(scope="module")
def ring_binary(tmp_path_factory):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("clang++ required for production IPC algorithm checks")
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/ipc/src/ipc.cppm").read_text()
    # Compile the actual exported declarations and methods, replacing only
    # platform atomics/IRQ control with host equivalents and a reservation probe.
    body = source[source.index("enum class MessageType") : source.index("// 零拷贝通道\n")]
    support = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
using usize=size_t; using MessageId=u64; using ThreadId=u32; using ProcessId=u32;
using std::atomic_thread_fence; using std::memory_order_release;
namespace intrinsics::memory { inline void *memcpy(void *d,const void *s,usize n){return std::memcpy(d,s,n);} }
static std::function<void()> reservation_probe;
namespace containers {
enum class MemoryOrder {Relaxed,Acquire,Release,AcqRel,SeqCst};
inline std::memory_order order(MemoryOrder o){
  switch(o){case MemoryOrder::Relaxed:return std::memory_order_relaxed;
  case MemoryOrder::Acquire:return std::memory_order_acquire;case MemoryOrder::Release:return std::memory_order_release;
  case MemoryOrder::AcqRel:return std::memory_order_acq_rel;default:return std::memory_order_seq_cst;}}
template<class T> struct AtomicCounter {
  std::atomic<T> value{0};
  T load(MemoryOrder o=MemoryOrder::SeqCst)const{return value.load(order(o));}
  void store(T v,MemoryOrder o=MemoryOrder::SeqCst){value.store(v,order(o));}
  T fetch_add(T v,MemoryOrder o=MemoryOrder::SeqCst){return value.fetch_add(v,order(o));}
  T fetch_sub(T v,MemoryOrder o=MemoryOrder::SeqCst){return value.fetch_sub(v,order(o));}
  bool compare_exchange_weak(T &e,T d,MemoryOrder o=MemoryOrder::SeqCst,MemoryOrder f=MemoryOrder::Acquire){
    bool ok=value.compare_exchange_weak(e,d,order(o),order(f));
    if(ok && reservation_probe){auto probe=std::move(reservation_probe);reservation_probe={};probe();} return ok;}
};
using AtomicU64=AtomicCounter<u64>; using AtomicU32=AtomicCounter<u32>;
struct IrqSpinLock {std::atomic_flag flag=ATOMIC_FLAG_INIT;
  void lock(){while(flag.test_and_set(std::memory_order_acquire))std::this_thread::yield();}
  void unlock(){flag.clear(std::memory_order_release);}};
template<class T> struct LockGuard {T &lock;explicit LockGuard(T &l):lock(l){lock.lock();}~LockGuard(){lock.unlock();}};
}
"""
    cases = r"""
int main(int argc,char **argv){
  assert(argc==2); const int which=std::atoi(argv[1]);
  alignas(64) u8 storage[64+2048]{};
  using Ring=ZeroCopyRingBuffer<1024,256>;
  MessageHeader h{},out{}; u8 data[192],received[192]; std::memset(data,0x59,sizeof(data));
  if(which==0){ // Consecutive 72-byte records place a header off its 64-byte alignment.
    Ring ring(storage,64+1024);h.payload_size=1;
    for(u64 i=0;i<40;i++){h.msg_id=i;assert(ring.try_send(h,data));
      assert(ring.try_receive(out,received,sizeof(received)));
      assert(out.msg_id==i && received[0]==0x59);} return 0;}
  if(which==1){ // Reported storage cannot expand the masked ring's physical capacity.
    Ring ring(storage,sizeof(storage));h.payload_size=192;
    for(int i=0;i<4;i++)assert(ring.try_send(h,data)); assert(!ring.try_send(h,data)); return 0;}
  if(which==2){ // A short extent used to underflow size - sizeof(control).
    Ring ring(storage,63);assert(!ring.try_send(h,data));return 0;}
  if(which==3){ // Observe exactly the old CAS reservation-before-copy window.
    Ring ring(storage,64+1024);h.payload_size=192;bool premature=false;
    reservation_probe=[&]{premature=ring.try_receive(out,received,sizeof(received));};
    assert(ring.try_send(h,data));assert(!premature);return 0;}
  if(which==4){ // Force a producer into the old reclamation-before-consumption window.
    Ring ring(storage,64+1024);h.payload_size=192;
    for(int i=0;i<4;i++)assert(ring.try_send(h,data));
    u8 replacement[192];std::memset(replacement,0x37,sizeof(replacement));
    reservation_probe=[&]{assert(ring.try_send(h,replacement));};
    assert(ring.try_receive(out,received,sizeof(received)));assert(received[0]==0x59);return 0;}
  if(which==5){Ring ring(storage,64+1024);h.payload_size=1;assert(!ring.try_send(h,nullptr));return 0;}
  if(which==7){ZeroCopyRingBuffer<1024,254> ring(storage,64+1024);h.payload_size=190;
    assert(!ring.try_send(h,data));return 0;} // Eight-byte padding counts toward the record budget.
  // Two writers and two readers share the real ring methods. Verify every
  // arbitrary ID exactly once, including records crossing the ring boundary.
  Ring ring(storage,64+1024);constexpr unsigned n=2000;
  std::atomic<unsigned> consumed{0};std::atomic<unsigned> seen[2*n]{};
  auto writer=[&](unsigned base){MessageHeader msg{};msg.payload_size=8;
    for(unsigned i=0;i<n;i++){u64 payload=base+i;msg.msg_id=payload;
      while(!ring.try_send(msg,&payload))std::this_thread::yield();}};
  auto reader=[&]{MessageHeader msg{};u64 payload;
    while(consumed.load()<2*n){if(ring.try_receive(msg,&payload,sizeof(payload))){
      assert(payload<2*n && msg.msg_id==payload);assert(seen[payload].fetch_add(1)==0);consumed.fetch_add(1);
    }else std::this_thread::yield();}};
  std::thread a(writer,0),b(writer,n),c(reader),d(reader);a.join();b.join();c.join();d.join();
  for(auto &count:seen)assert(count.load()==1);assert(ring.get_statistics().pending_messages==0);
}
"""
    directory = tmp_path_factory.mktemp("ipc-production")
    cpp = directory / "ring.cpp"
    binary = directory / "ring"
    cpp.write_text(support + body + cases)
    result = subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-O1",
            "-pthread",
            "-fsanitize=undefined,alignment",
            "-fno-sanitize-recover=all",
            str(cpp),
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


@pytest.mark.parametrize("case", range(8))
def test_production_ring(ring_binary, case):
    result = subprocess.run([str(ring_binary), str(case)], capture_output=True, text=True, timeout=12, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


def test_channel_timeout_clock_uses_calibrated_nanoseconds(tmp_path):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("clang++ required for production IPC clock check")
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/ipc/src/ipc.cppm").read_text()
    begin = source.index("[[nodiscard]] static u64 get_current_time_ns()")
    method = source[begin : source.index("}", begin) + 1]
    cpp = tmp_path / "channel-clock.cpp"
    cpp.write_text(
        "#include <cassert>\n#include <cstdint>\nusing u64=uint64_t;\n"
        "namespace arch { u64 get_timestamp_counter(){return 62500;} }\n"
        "namespace timer { struct TimerSubsystem { u64 ns=1000000;"
        "static TimerSubsystem& instance(){static TimerSubsystem clock;return clock;}"
        "u64 now_ns()const{return ns;} }; }\n"
        "struct Channel {" + method + "};\n"
        "int main(){assert(Channel::get_current_time_ns()==1000000);"
        "timer::TimerSubsystem::instance().ns=2000000;"
        "assert(Channel::get_current_time_ns()==2000000);}\n"
    )
    binary = tmp_path / "channel-clock"
    built = subprocess.run([compiler, "-std=c++23", str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr


def test_service_removal_during_connect_cannot_publish_orphan(tmp_path):
    compiler = shutil.which("clang++")
    if not compiler:
        pytest.skip("clang++ required for production IPC lifecycle check")
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/ipc/src/ipc.cppm").read_text()
    start = source.index("[[nodiscard]] KernelResult<ChannelId> connect_to_service(")
    method = source[start : source.index("\n  }\n", start) + 4]
    cpp = tmp_path / "service-lifecycle.cpp"
    cpp.write_text(
        r"""
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
using u32 = std::uint32_t;
using ServiceId = u32;
using ChannelId = u32;
using ProcessId = u32;
enum class KernelError { NotFound, ResourceExhausted, OutOfMemory };
struct VoidResult {
  bool ok = true;
  KernelError failure = KernelError::NotFound;
  explicit operator bool() const { return ok; }
  KernelError error() const { return failure; }
};
template<class T> struct KernelResult {
  bool ok;
  T value{};
  KernelError failure = KernelError::NotFound;
  KernelResult(T n) : ok(true), value(n) {}
  KernelResult(KernelError e) : ok(false), failure(e) {}
  explicit operator bool() const { return ok; }
};
namespace moss { using std::move; }
using std::make_shared;
using std::shared_ptr;
namespace containers {
enum class MemoryOrder { Relaxed, Acquire, AcqRel };
using IrqSpinLock = std::mutex;
template<class T> using LockGuard = std::lock_guard<T>;
template<class T> struct AtomicCounter {
  T value;
  AtomicCounter(T n = 0) : value(n) {}
  T load(MemoryOrder) const { return value; }
  T fetch_add(T n, MemoryOrder) { T old = value; value += n; return old; }
};
}
template<class K, class V> struct Map {
  std::unordered_map<K,V> entries;
  std::optional<V> find(K key) const {
    auto it = entries.find(key);
    return it == entries.end() ? std::nullopt : std::optional<V>(it->second);
  }
  void insert_or_update(K key, V value) { entries[key] = std::move(value); }
  void remove(K key) { entries.erase(key); }
};
struct ServiceDescriptor {
  ServiceId service_id = 7;
  ProcessId provider_pid = 1;
  u32 max_clients = 1;
  containers::AtomicCounter<u32> current_clients;
};
struct ConnectionDescriptor {
  ConnectionDescriptor(ChannelId, ProcessId, ProcessId, ServiceId) {}
};
std::function<void()> during_initialize;
struct ZeroCopyChannel {
  ZeroCopyChannel(ChannelId, ProcessId, ProcessId) {}
  VoidResult initialize() { if (during_initialize) during_initialize(); return {}; }
};
struct IpcManager {
  containers::IrqSpinLock lifecycle_lock_;
  Map<ServiceId, shared_ptr<ServiceDescriptor>> services_;
  Map<ChannelId, shared_ptr<ZeroCopyChannel>> channels_;
  Map<ChannelId, shared_ptr<ConnectionDescriptor>> connections_;
  containers::AtomicCounter<ChannelId> next_channel_id_{1};
  containers::AtomicCounter<u32> total_channels_;
  void add_process_channel(ProcessId, ChannelId) {}
"""
        + method
        + r"""
};
int main() {
  IpcManager manager;
  auto service = make_shared<ServiceDescriptor>();
  manager.services_.insert_or_update(service->service_id, service);
  during_initialize = [&] { manager.services_.remove(service->service_id); };
  const auto removed = manager.connect_to_service(2, service->service_id);
  assert(!removed && manager.channels_.entries.empty() && manager.connections_.entries.empty());
  assert(service->current_clients.load(containers::MemoryOrder::Relaxed) == 0);
  assert(manager.total_channels_.load(containers::MemoryOrder::Relaxed) == 0);
  during_initialize = {};
  manager.services_.insert_or_update(service->service_id, service);
  const auto connected = manager.connect_to_service(2, service->service_id);
  assert(connected && manager.channels_.entries.size() == 1 && manager.connections_.entries.size() == 1);
  assert(service->current_clients.load(containers::MemoryOrder::Relaxed) == 1);
  assert(!manager.connect_to_service(3, service->service_id));
}
"""
    )
    binary = tmp_path / "service-lifecycle"
    built = subprocess.run([compiler, "-std=c++23", str(cpp), "-o", str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stdout + result.stderr
