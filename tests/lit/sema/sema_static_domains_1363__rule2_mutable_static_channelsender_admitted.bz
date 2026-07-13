// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_channelsender_admitted).
//
// (20) §22.12 rule 2 form (i): ChannelSender<T> IS in the synchronized set —
// a mutable static ChannelSender<int>? is admitted (the sanctioned
// process-global send endpoint that replaces a static Channel<T>).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class App public static _tx: ChannelSender<int>?; public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
