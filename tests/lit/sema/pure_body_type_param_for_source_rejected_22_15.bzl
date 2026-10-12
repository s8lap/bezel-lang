// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §22.15 rule 5: a bare generic-type-parameter for-source is rejected in a
// pure body. The type parameter's headName() is its own name ("T"), which
// matches no arm in isEnumeratorConstructingSource, so it fell through to
// `return false` (admitted) and the for died in codegen with "not yet
// implemented: codegen for this for statement". The fix adds the
// Named+null-declaration+no-generic-args sentinel to catch it.
//
// The fix MUST NOT reject an instantiated builtin type (OwningBuffer<Dog>,
// etc.) whose declaration is also null — distinguished by having generic
// args. The OwningBuffer<Dog> case is covered by the gtest
// pure_class_typed_owning_buffer_for_accepted_spec_22_15 (runAot).

module M;

public class Container<T> where T: IEnumerable<int>
  public static pure method Sum(source: T): int
    var total := 0;
    for var x in source do
      total := total + x;
    end;
    return total;
  end Sum;
end Container;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `for` loop source is not admitted in a pure body — the source constructs an enumerator (an IEnumerable<T>-typed source, or an `Array<T>` whose element is a type parameter); declare the source at a concrete sequence type, or use an index loop `for var i in 0..<xs.Count` (§22.15)
