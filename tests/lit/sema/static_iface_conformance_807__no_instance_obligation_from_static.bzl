// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, no_instance_obligation_from_static).
//
// A static interface member creates NO instance obligation: the conforming
// class needs no instance `Zero` (would otherwise trip §12.5's abstract check).
//
// ACCEPT control: bezelc -c must exit zero. v0.201 (#2048): the fresh-producing
// static members are spelled `owned T` / `owned W` per the declaration-site
// static-return rule.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NO-ERROR --allow-empty %s

module M; public interface IZ<T> where T: IZ<T> static method Zero(): owned T; method Scale(k: int): int; end IZ; public class W: IZ<W> public static method Zero(): owned W => new W(); public method Scale(k: int): int => k; end W;

// NO-ERROR-NOT: error:
