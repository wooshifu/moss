#pragma once

#include <mlibc/sysdep-signatures.hpp>
#include <sys/sysinfo.h>

// Moss reuses this public structure without enabling the Linux syscall profile.
#if !__MLIBC_LINUX_OPTION
struct Sysinfo {};
#endif
namespace mlibc {
#if !__MLIBC_LINUX_OPTION
template <> struct SysdepImpl<Sysinfo> {
  static int operator()(struct ::sysinfo *info);
};
#endif
struct MossSysdepTags : LibcPanic,
                        LibcLog,
                        Write,
                        Isatty,
                        TcbSet,
                        AnonAllocate,
                        AnonFree,
                        Seek,
                        Exit,
                        Close,
                        FutexWake,
                        FutexWait,
                        Read,
                        Pipe,
                        Dup,
                        Dup2,
                        Fcntl,
                        Open,
                        Stat,
                        Access,
                        GetCwd,
                        Chdir,
                        Mkdir,
                        Rmdir,
                        Unlinkat,
                        Rename,
                        OpenDir,
                        ReadEntries,
                        VmMap,
                        VmUnmap,
                        ClockGet,
                        Sleep,
                        Uname,
                        Sysinfo,
                        Sysconf,
                        GetAffinity,
                        SetAffinity,
                        GetPid,
                        GetPpid,
                        GetPgid,
                        GetSid,
                        SetPgid,
                        SetSid,
                        GetUid,
                        GetEuid,
                        GetGid,
                        GetEgid,
                        Kill,
                        Sigaction,
                        Sigprocmask,
                        Yield,
                        Fork,
                        Waitpid,
                        Execve {};
template <typename Tag> using Sysdeps = SysdepOf<MossSysdepTags, Tag>;
struct SysdepTraits {
  static constexpr bool usesRtNetlink = false;
};
} // namespace mlibc
