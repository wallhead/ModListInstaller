# Sliding Verified Read-Ahead

## Scope

Correct the verified input scheduler confirmed in review. Keep full SHA-256,
the four-block forward window, the six-buffer budget, split volumes, output
behavior and the four-thread decoder setting. Do not publish or commit.

## Approach

Extract the existing queue/cache policy into an internal production helper used
by VerifiedArchiveStream. Test that same helper deterministically, then change
its policy. Moving the enqueue loop alone would retain the faulty LRU eviction;
increasing the cache would only postpone the problem.

The corrected policy advances on each consumed chunk, discards obsolete queued
speculation after seeks, promotes demand, and tracks one in-flight load. Evict
consumed/obsolete buffers first, preserving the demand and useful forward window.
Never evict borrowed buffers. Reserve space before starting a load so cached,
borrowed and loading buffers together stay within six chunk buffers. Disk I/O,
hashing and callbacks remain outside the queue mutex.

Final verification drains every unverified chunk independently of the prefetch
window, with bounded buffer use. Cancellation must release idle/waiting threads
and safely cancel/drain overlapped reads, including destruction during a read.

## Validation

Exercise sequential access across more than six chunks, the unread-chunk eviction
regression, demand promotion, joining in-flight reads, seeks during loading,
borrowed-buffer backpressure, final verification, boundaries and short tails.
Retain real split-archive payload/corruption tests and add cancellation coverage.
No speedup is claimed without a paired performance measurement.
