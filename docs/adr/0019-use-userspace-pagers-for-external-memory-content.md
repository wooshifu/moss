---
status: accepted
date: 2026-09-19
---

# Use Userspace Pagers for External Memory Content

Moss will represent file-backed and other externally supplied mappings as Pager-Backed Memory Objects. On a missing-page fault, the Mechanism Kernel Domain validates access, blocks the faulting thread and requests content from the object's authorized Pager Service. The pager interprets the object offset, obtains the content and supplies it through a kernel-controlled mechanism.

The kernel owns physical-frame allocation, page-table installation, mapping permissions, resident-page lifetime, dirty-state observation, global memory accounting and selection of reclaim candidates. The pager owns logical content identity, backing-store access, coherence, truncation and writeback policy. Processes sharing the same Pager-Backed Memory Object observe the kernel-managed resident pages for that object; the kernel does not identify them by pathname, inode or filesystem format.

## Considered Options

A kernel page cache keyed by filesystem objects was rejected because it would recreate file identity and filesystem callbacks inside the Mechanism Kernel Domain. Giving a userspace pager direct control over physical frames or page tables was rejected because it would bypass memory isolation and global accounting. Eagerly copying complete files into anonymous mappings was rejected because it cannot provide scalable shared mappings, demand paging or coordinated writeback.

## Consequences

Moss needs pager registration, bounded page requests, validated content supply, dirty-page and writeback handshakes, reclaim coordination, range invalidation and deterministic behavior when a pager dies. Pager execution must not depend recursively on an object it is responsible for supplying. Tests must cover shared mappings, permission failures, truncation races, dirty writeback, memory pressure, pager failure and cleanup. Read-ahead sizes, eviction algorithms, supply batching and the process-visible result of pager failure remain separate decisions.
