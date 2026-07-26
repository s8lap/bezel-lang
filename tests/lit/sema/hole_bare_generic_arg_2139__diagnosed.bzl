// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2139 (§18.5, spec v0.226): the bare `Identifier<T1, T2>` (no `.Member` /
// `(args)` after the `>`) was silently falling through the §18.5 hole-cut
// recognizer. The earlier #2102 fix only fired when `.`/`(` followed the `>`,
// confirming a member access; without that follow-up, the bare shape was
// parsed as a relational chain (`Cache < int`) and the user saw
// "operator `<` is not defined for type `Cache`" instead of the §18.5 +
// parenthesization-remedy message. The fix broadens the recognizer to fire
// when the `>` is the last token of the fragment; the §18.5 rule and the
// documented `{(Expr<T1, T2>.Member)}` parenthesization remedy now name the
// actual construct the user wrote.

module M;
class Registry<T, U>
  public static Default: int := 42;
end Registry;
class C
  public static method Main(): int
    var s := $"{Registry<int, string>}";
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: interpolation hole's `,` falls inside what looks like an unparenthesized generic-argument list
// CHECK-SAME: parenthesize
// CHECK-SAME: (§18.5
