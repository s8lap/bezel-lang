// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #4655 (§5.1 "Implicit conversions", v0.307): the §5.1 implicit-
// conversion REJECT diagnostic (`ExprTC::checkAssignable`'s
// `reportNotConvertible`, src/sema/ExprTypecheck.cpp) named only the generic
// "(§5.1)" citation for every sub-rule it enforces, so a case whose expected
// wording quotes the spec's OWN phrase for the specific rule that fired
// never matched the compiler's actual output — even though the compiler
// correctly rejected every one of these five shapes. Found by a corpus
// sweep (#4654-family) over tests/conformance/rules/cases/s5_1_*.bzl:
// s5_1_0001/0003 (signed/unsigned), s5_1_0009 (decimal/binary float),
// s5_1_0011 (enum/underlying-int), s5_1_0012 (integer/bool), s5_1_0015
// (constant narrowing). This file pins that the diagnostic now carries the
// spec's own wording for each of the five distinct closed-form rules the
// generic message used to collapse into one indistinguishable sentence.
// Pre-fix, every "CHECK-SAME" line below is absent from the compiler's
// output (only the generic "(§5.1)" is present); post-fix each is present.
//
// Programs below are ported 1:1 from the corpus cases named in each comment
// (module/class wrapper compacted to this suite's usual one-line lit style;
// the checked program text is unchanged).

module M;

// Ported from tests/conformance/rules/cases/s5_1_0003.bzl. spec §5.1:
// "Between signed and unsigned types of the same or narrower width (e.g.
// `int -> uint`, `uint -> int`, `short -> ushort`)."
class SU public static method Convert(n: int) var u: uint := n; end Convert; end SU;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `u` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1): between signed and unsigned types of the same or narrower width is never implicit

// Ported from tests/conformance/rules/cases/s5_1_0009.bzl. spec §5.1:
// "Between `decimal` and binary floats (`float`, `double`)."
class DF public static method Convert(d: double) var m: decimal := d; end Convert; end DF;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `m` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1): between `decimal` and binary floats (`float`, `double`) is never implicit

// Ported from tests/conformance/rules/cases/s5_1_0011.bzl. spec §5.1:
// "Between an enum and its underlying integer."
enum Priority Low, Medium, High; end Priority;
class EI public static method Convert(p: Priority) var n: int := p; end Convert; end EI;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `n` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1): between an enum and its underlying integer is never implicit

// Ported from tests/conformance/rules/cases/s5_1_0012.bzl. spec §5.1:
// "Between integers and `bool`."
class IB public static method Convert(n: int) var b: bool := n; end Convert; end IB;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `b` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1): between integers and `bool` is never implicit

// Ported from tests/conformance/rules/cases/s5_1_0015.bzl. spec §5.1
// "Constant narrowing": "A constant whose value does not fit the target's
// range is a compile error at the conversion site — never a silent
// truncation."
class CN public static method Make() var b: byte := 300; end Make; end CN;
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the initializer for `b` has a type that is not implicitly convertible to the target type
// CHECK-SAME: (§5.1): a constant whose value does not fit the target's range is a compile error at the conversion site
