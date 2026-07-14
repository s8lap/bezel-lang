// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Regression for #1645 (stdlib §4.13, v0.196) — POSITIVE control.
//
// The §4.13 element-class constraint applies ONLY to the OWNING enumeration
// interfaces. The plain (borrowing) `IEnumerable<T>` carries no such constraint,
// so a value-element `IEnumerable<int>` iterator must keep compiling — the fix
// must not spill over onto the borrowing iteration family. // per stdlib §4.13

module M;
class Foo
  public method Gen(): IEnumerable<int>
    yield return 1;
  end Gen;
end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
