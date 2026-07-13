// Ported from tests/sema/channel_640_phantom_members_test.cpp
//   (BEZEL_TEST channel_640_phantom_members, channel_tryreceive_still_allowed_6_9).
//
// ---------------------------------------------------------------------------
// (7) §6.9 / §30.15: ch.TryReceive(out v) — legitimate method on Channel<T>; no error.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading; class C public static method Test() using var ch := new Channel<int>(); var v: int? := null; var ok := ch.TryReceive(out v); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
