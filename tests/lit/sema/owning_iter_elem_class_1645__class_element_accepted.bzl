// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Regression for #1645 (stdlib §4.13, v0.196) — POSITIVE control.
//
// The §4.13 element-class rule rejects only NON-class element types. An owning
// iterator whose element type is a proper class (`Res`) is well-formed and must
// still compile cleanly, through codegen — the fix must NOT over-reject it.
// per stdlib §4.13

module M;
class Res destructor end destructor; end Res;
class Foo
  public method Gen(): IOwningEnumerable<Res>
    yield return new Res();
  end Gen;
end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
