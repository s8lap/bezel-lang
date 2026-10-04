// Issue #5344 / §5.3 (v0.314): `bool?(int?)` must be admitted.
// Per §5.3's general rule: `T?(x)` is admitted exactly when the
// corresponding non-nullable conversion `T(v)` is admitted. Since
// `bool(int)` IS permitted (int-to-bool is total, information-preserving:
// false→0, true→1), `bool?(int?)` IS admitted too.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public static method Main(): int
    var i: int? := 42;
    var b: bool? := bool?(i);
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
