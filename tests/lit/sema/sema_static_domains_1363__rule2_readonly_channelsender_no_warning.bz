// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_readonly_channelsender_no_warning).
//
// (26) §22.12 rule 4 (v0.181, #1556): a mutable static ChannelSender<T> in the
// synchronized set draws NO rule-4 warning and NO rule-2 error — the form-(i)
// suppression applies to the send endpoint.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class App public static readonly _tx: ChannelSender<int>?; public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
