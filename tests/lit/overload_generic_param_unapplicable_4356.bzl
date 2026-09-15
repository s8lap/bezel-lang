// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #4356 (§22.6, v0.298): when resolveOverload encounters an unjudgeable
// parameter type (a generic type like IVal<T> with unbound type parameters),
// it returned Undecidable BEFORE calling classifyArg for that overload. This
// left applicable[] at its initial true state for ALL overloads, causing
// arityFallback to select the first arity-matching overload even when
// classifyArg would have found it NotApplicable. The wrong overload then
// reached codegen and produced a LLVM IR verification error
// ("Call parameter type does not match function signature").
//
// The fix: when a parameter type is unjudgeable, mark that candidate
// inapplicable and skip its classification (instead of returning early),
// so that applicable[] correctly reflects which overloads were found
// NotApplicable. arityFallback then avoids selecting an overload whose
// classifyArg result was NotApplicable.
//
// This test verifies the correct semantic rejection: when neither overload
// is applicable, a proper diagnostic is emitted instead of a codegen crash.

module OverloadGeneric4356;

// An interface with a type parameter.
interface IVal<T> method get(): int; end IVal;

// A plain record - not a Widget.
record Widget(val: int);

// A plain record - unrelated to Widget.
record WidgetBox(v: int);

// Two overloads: one concrete, one generic.
class Overloader
    public static method Process(w: Widget): int => w.val;
    public static method Process<T>(b: IVal<T>): int => b.get();
end Overloader;

class Test
    public static method CallWithUnrelated(): int
        var wb := new WidgetBox(20);
        // WidgetBox is neither a Widget nor an IVal<T> — neither overload
        // is applicable. With the fix, this is rejected with a proper
        // semantic error (§22.6), not a codegen crash.
        var r: int := Overloader.Process(wb);
        return r;
    end CallWithUnrelated;

    public static method Main(): int
        return 0;
    end Main;
end Test;

// CHECK-DAG: :[[@LINE-9]]:{{[0-9]+}}: no overload of `Process` is applicable to these arguments; every candidate would require a conversion that §5.1 does not admit as implicit (§22.6)
