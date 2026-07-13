// Ported from tests/sema/channel_640_phantom_members_test.cpp
//   (BEZEL_TEST channel_640_phantom_members, channel_receive_still_allowed_6_9).
//
// ---------------------------------------------------------------------------
// (6) §6.9 / §30.15: ch.Receive() — legitimate async method on Channel<T>; no error.
// (Receive returns an OwningTask/Task; just type-check, don't run.)
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading; class C public static async method Test(): Task using var ch := new Channel<int>(); var _ := await ch.Receive(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
