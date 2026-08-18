// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for #3109 narrow re-attempt (v0.256). The diagnostic text
// pin — `char mod char` is the rejection shape, citing §6.1.

module M;
class C
  public static method Main(): int
    var c: char := 'a';
    var d: char := 'b';
    return int(c mod d);
  end Main;
end C;

// CHECK: `mod` is not defined on `char` operands
// CHECK: §6.1
// CHECK: `int(c) mod int(d)`
// CHECK: §5.2
