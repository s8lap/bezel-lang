// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, owned_array_sendable_element_as_channel_element_accepted_30_14).
//
// §30.14 stdlib table: OwningArray<SealedSendable> → sendable (all state owned).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Item public Val: int := 0; destructor end destructor; end Item; class C public static method Test() using var ch := new Channel<OwningArray<Item>>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
