// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_position, spec_12_9_producer_consumer_pair_accepted).
//
// §12.9: two separate interfaces, one `out` (IProducer) and one `in`
// (IConsumer), each with T in the correct position — both must be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IProducer<out T> method Produce(): T; end IProducer; interface IConsumer<in T> method Consume(item: T); end IConsumer;

// NODIAG-NOT: error
// NODIAG-NOT: §
