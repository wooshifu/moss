#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {
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
                        GetPid,
                        GetPpid,
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
