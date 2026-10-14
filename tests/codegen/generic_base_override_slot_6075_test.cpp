// Codegen — §12.1 / §14.1 / §27.8 (v0.338): an override of a generic base's
// virtual member, spelled with the base's type ARGUMENT, must reuse the base's
// vtable slot.
//
// Pre-fix (src/codegen/LlvmBackend_Setup.cpp, Emitter::buildVtable), the base
// `G<T>` keyed its member by the declared parameter `T`, and the override
// `Dv: G<int>` keyed its member by `int`. The keys differed, so the override
// got a new slot and a base-typed call dispatched the base body. The indexer
// probes printed `1` (expected `2`), and so did the method row.
//
// Rows (runAot; the observable is captured stdout):
//   - indexer over `[i: T]`, overridden with `[i: int]`
//   - indexer over `[i: T]`, overridden with an alias `[i: Idx]` (`Idx := int`)
//   - method over `Foo(x: T)`, overridden with `Foo(x: int)`
//   - control: a non-generic base with the same override, unchanged
//   - nested substitution (Array<T>, Array<T?>, Box<T>, Array<Array<T>>, function,
//     owned/ref, alias spellings) through D: G<int>, D<U>: G<U>, D<U>: G<Stack<U>>,
//     multi-level chains, and the interface view (see the section further down)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// §12.1 / §27.8 v0.338: base-typed indexer call reaches the override. Pre-fix
// stdout is "1\n".
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_generic_base_param_overridden_with_argument_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class G<T> "
        "  public virtual property [i: T]: int "
        "    get => 1; "
        "  end; "
        "end G; "
        "public class Dv: G<int> "
        "  public override property [i: int]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: G<int> := new Dv(); "
        "    Console.WriteLine(b[1].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §27.8 v0.338: the same, with the override spelled through an alias
// (`type Idx := int`). Pre-fix stdout is "1\n".
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_generic_base_param_overridden_with_alias_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "type Idx := int; "
        "public class G<T> "
        "  public virtual property [i: T]: int "
        "    get => 1; "
        "  end; "
        "end G; "
        "public class Dv: G<int> "
        "  public override property [i: Idx]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: G<int> := new Dv(); "
        "    Console.WriteLine(b[1].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §12.5 v0.338: a base-typed method call over a generic base's `T`
// parameter reaches the override spelled with the argument. Pre-fix stdout is
// "1\n".
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_generic_base_param_overridden_with_argument_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class G<T> "
        "  public virtual method Foo(x: T): int "
        "    return 1; "
        "  end Foo; "
        "end G; "
        "public class Dv: G<int> "
        "  public override method Foo(x: int): int "
        "    return 2; "
        "  end Foo; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: G<int> := new Dv(); "
        "    Console.WriteLine(b.Foo(1).ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 v0.338 control: a non-generic base with an `int` override. Unchanged by
// the fix; stdout is "2\n".
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           control_nongeneric_base_int_override_12_1_v0338) {
    auto r = runAot(
        "module Al; "
        "public class B "
        "  public virtual property [i: int]: int "
        "    get => 1; "
        "  end; "
        "end B; "
        "public class Dv: B "
        "  public override property [i: int]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    Console.WriteLine(b[1].ToString()); "
        "  end Main; "
        "end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// ---------------------------------------------------------------------------
// Nested substitution (fix-forward of the first #6075 revision). The base member
// is keyed by the declared parameter type, so a parameter that merely CONTAINS
// `T` (`Array<T>`, `Array<T?>`, `Box<T>`, `Array<Array<T>>`) must be keyed with
// `T` replaced at ANY depth, through `D: G<int>`, `D<U>: G<U>`,
// `D<U>: G<Stack<U>>` and multi-level chains, or the override gets a new slot
// and a base-typed call runs the base body. Each program below prints the result
// of a base-typed, a derived-typed and an interface-typed call; every one must
// reach the most-derived body. The sources come from the probe matrix
// /var/tmp/bezel-scratch/r30-p12-s-6075fix/genmatrix.py.
// ---------------------------------------------------------------------------

// §12.1 / §14.1 / §27.8 v0.338: base-typed, derived-typed and interface-typed calls of `Foo(x: Array<T>)` overridden as `Array<int>` in `D: G<int>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_overridden_in_int_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: the indexer `[i: Array<T>]` overridden as `[i: Array<int>]`, including the interface view (itable wiring).
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_t_overridden_in_int_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<T>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<T>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override property [i: Array<int>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b[new Array<int>(1)].ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d[new Array<int>(1)].ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i[new Array<int>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<U>: G<U>` overriding `Foo(x: Array<T>)` as `Array<U>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_overridden_in_type_parameter_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<U>\n"
        "  public override method Foo(x: Array<U>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString());\n"
        "    var i: IG<int> := new D<int>();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<U>: G<U>` overriding `[i: Array<T>]` as `[i: Array<U>]`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_t_overridden_in_type_parameter_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<T>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<T>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D<U>: G<U>\n"
        "  public override property [i: Array<U>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    Console.WriteLine(b[new Array<int>(1)].ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d[new Array<int>(1)].ToString());\n"
        "    var i: IG<int> := new D<int>();\n"
        "    Console.WriteLine(i[new Array<int>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<U>: G<Stack<U>>`: the argument itself contains the derived type parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_overridden_in_nested_argument_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<Stack<U>>\n"
        "  public override method Foo(x: Array<Stack<U>>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Array<Stack<int>>(1)).ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d.Foo(new Array<Stack<int>>(1)).ToString());\n"
        "    var i: IG<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(i.Foo(new Array<Stack<int>>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<U>: G<Stack<U>>` over an indexer.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_t_overridden_in_nested_argument_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<T>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<T>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D<U>: G<Stack<U>>\n"
        "  public override property [i: Array<Stack<U>>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(b[new Array<Stack<int>>(1)].ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d[new Array<Stack<int>>(1)].ToString());\n"
        "    var i: IG<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(i[new Array<Stack<int>>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: a generic class `Box<T>` parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_generic_class_of_t_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Box<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Box<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<U>\n"
        "  public override method Foo(x: Box<U>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Box<int>()).ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d.Foo(new Box<int>()).ToString());\n"
        "    var i: IG<int> := new D<int>();\n"
        "    Console.WriteLine(i.Foo(new Box<int>()).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `Array<T?>` parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_nullable_argument_array_of_t_nullable_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T?>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T?>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int?>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<int?>(1)).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Array<int?>(1)).ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(new Array<int?>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `Array<Array<T>>` at depth two, indexer and interface view.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_array_of_t_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<Array<T>>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<Array<T>>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override property [i: Array<Array<int>>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b[new Array<Array<int>>(1)].ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d[new Array<Array<int>>(1)].ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i[new Array<Array<int>>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `Array<Array<T>>` under `G<Stack<U>>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_array_of_t_stack_argument_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<Array<T>>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<Array<T>>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<Stack<U>>\n"
        "  public override method Foo(x: Array<Array<Stack<U>>>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Array<Array<Stack<int>>>(1)).ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d.Foo(new Array<Array<Stack<int>>>(1)).ToString());\n"
        "    var i: IG<Stack<int>> := new D<int>();\n"
        "    Console.WriteLine(i.Foo(new Array<Array<Stack<int>>>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: a function-typed `(T) -> T` parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_function_typed_parameter_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method Foo(f: (T) -> T): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(f: (int) -> int): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(lambda(x: int) => x).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `(T) -> T` under `D<U>: G<U>`, base, derived and interface views.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_function_typed_parameter_type_parameter_derived_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: (T) -> T): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: (T) -> T): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<U>\n"
        "  public override method Foo(x: (U) -> U): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(lambda(x: int) => x).ToString());\n"
        "    var d := new D<int>();\n"
        "    Console.WriteLine(d.Foo(lambda(x: int) => x).ToString());\n"
        "    var i: IG<int> := new D<int>();\n"
        "    Console.WriteLine(i.Foo(lambda(x: int) => x).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: an `owned x: T` parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_owned_parameter_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(owned x: T): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(owned x: T): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<Item>\n"
        "  public override method Foo(owned x: Item): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<Item> := new D();\n"
        "    Console.WriteLine(b.Foo(new Item()).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Item()).ToString());\n"
        "    var i: IG<Item> := new D();\n"
        "    Console.WriteLine(i.Foo(new Item()).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: a `ref x: T` parameter.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_ref_parameter_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(ref x: T): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(ref x: T): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(ref x: int): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var v: int := 1;\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(ref v).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(ref v).ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(ref v).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: bare `T` overridden through `type I := int`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_bare_t_through_alias_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "type I := int;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: T): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: I): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(1).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(1).ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(1).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: the override spelled with `type Ints := Array<int>` (h7).
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_through_alias_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "type Ints := Array<int>;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Ints): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: alias-spelled `[i: Ints]` over `[i: Array<T>]`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_t_through_alias_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "type Ints := Array<int>;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<T>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<T>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override property [i: Ints]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b[new Array<int>(1)].ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d[new Array<int>(1)].ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i[new Array<int>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: the intermediate class overrides and so does the most derived (`D2: D: G<int>`).
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_two_level_override_again_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "end D;\n"
        "public class D2: D\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "end D2;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D2();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var d := new D2();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString());\n"
        "    var i: IG<int> := new D2();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: the same over an indexer.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           indexer_array_of_t_two_level_override_again_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  readonly property [i: Array<T>]: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual property [i: Array<T>]: int\n"
        "    get => 1;\n"
        "  end;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override property [i: Array<int>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D;\n"
        "public class D2: D\n"
        "  public override property [i: Array<int>]: int\n"
        "    get => 2;\n"
        "  end;\n"
        "end D2;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D2();\n"
        "    Console.WriteLine(b[new Array<int>(1)].ToString());\n"
        "    var d := new D2();\n"
        "    Console.WriteLine(d[new Array<int>(1)].ToString());\n"
        "    var i: IG<int> := new D2();\n"
        "    Console.WriteLine(i[new Array<int>(1)].ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: the override set `Foo(Array<T>)` / `Foo(string)`; all three views reach the most-derived body (the interface view by the parameter-type witness match of #5999, kept off the interface's own `T`).
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           overload_set_with_nongeneric_sibling_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "  method Foo(x: string): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "  public virtual method Foo(x: string): int return 5; end Foo;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "  public override method Foo(x: string): int return 7; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString() + \" \" + b.Foo(\"s\").ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString() + \" \" + d.Foo(\"s\").ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString() + \" \" + i.Foo(\"s\").ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2 7\n2 7\n2 7\n");
}

// §12.1 / §14.1 / §27.8 v0.338: a virtual property next to a re-keyed method keeps its own slot.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           property_beside_array_of_t_method_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Box<T> end Box;\n"
        "public class Item end Item;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "  readonly property P: int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "  public virtual property P: int get => 1; end;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "  public override property P: int get => 2; end;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString() + \" \" + b.P.ToString());\n"
        "    var d := new D();\n"
        "    Console.WriteLine(d.Foo(new Array<int>(1)).ToString() + \" \" + d.P.ToString());\n"
        "    var i: IG<int> := new D();\n"
        "    Console.WriteLine(i.Foo(new Array<int>(1)).ToString() + \" \" + i.P.ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2 2\n2 2\n2 2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `M(Array<T>)` and `M(Array<string>)` are distinct slots in `D: G<int>` (#6074 purpose preserved).
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           methods_array_of_t_and_array_of_string_stay_apart_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method M(x: Array<T>): int return 1; end M;\n"
        "  public virtual method M(x: Array<string>): int return 5; end M;\n"
        "end G;\n"
        "public class Dv: G<int>\n"
        "  public override method M(x: Array<int>): int return 2; end M;\n"
        "end Dv;\n"
        "public class Dw: G<string>\n"
        "  public override method M(x: Array<string>): int return 7; end M;\n"
        "end Dw;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new Dv();\n"
        "    Console.WriteLine(b.M(new Array<int>(1)).ToString());\n"
        "    Console.WriteLine(b.M(new Array<string>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n5\n");
}

// §12.1 / §14.1 / §27.8 v0.338: interface-implemented virtual `Foo(Array<T>)` overridden in `D: G<int>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_array_of_t_via_interface_view_h5_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public interface IG<T>\n"
        "  method Foo(x: Array<T>): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class Dv: G<int>\n"
        "  public override method Foo(x: Array<int>): int return 2; end Foo;\n"
        "end Dv;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: IG<int> := new Dv();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var c: G<int> := new Dv();\n"
        "    Console.WriteLine(c.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: override only the `Array<string>` overload; `Array<T>` keeps the base body.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           overloads_array_of_t_array_of_string_override_one_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method M(x: Array<T>): int return 1; end M;\n"
        "  public virtual method M(x: Array<string>): int return 5; end M;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method M(x: Array<string>): int return 7; end M;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.M(new Array<int>(1)).ToString());\n"
        "    Console.WriteLine(b.M(new Array<string>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "1\n7\n");
}

// §12.1 / §14.1 / §27.8 v0.338: override both overloads.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           overloads_array_of_t_array_of_string_override_both_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method M(x: Array<T>): int return 1; end M;\n"
        "  public virtual method M(x: Array<string>): int return 5; end M;\n"
        "end G;\n"
        "public class D: G<int>\n"
        "  public override method M(x: Array<string>): int return 7; end M;\n"
        "  public override method M(x: Array<int>): int return 2; end M;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D();\n"
        "    Console.WriteLine(b.M(new Array<int>(1)).ToString());\n"
        "    Console.WriteLine(b.M(new Array<string>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n7\n");
}

// §12.1 / §14.1 / §27.8 v0.338: several parameters that mention `T`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_two_parameters_array_bare_stack_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method Foo(a: Array<T>, b: T, c: Stack<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D: G<string>\n"
        "  public override method Foo(a: Array<string>, b: string, c: Stack<string>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<string> := new D();\n"
        "    Console.WriteLine(b.Foo(new Array<string>(1), \"s\", new Stack<string>()).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `G<T>` <- `M<U>: G<U>` <- `D<V>: M<V>` <- `E: D<int>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           three_level_chain_through_generic_middle_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class M<U>: G<U>\n"
        "end M;\n"
        "public class D<V>: M<V>\n"
        "  public override method Foo(x: Array<V>): int return 2; end Foo;\n"
        "end D;\n"
        "public class E: D<int>\n"
        "end E;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new E();\n"
        "    Console.WriteLine(b.Foo(new Array<int>(1)).ToString());\n"
        "    var m: M<int> := new E();\n"
        "    Console.WriteLine(m.Foo(new Array<int>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<P,Q>: G<Q,P>` over `Dictionary<A,B>`: positional substitution.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_swapped_type_arguments_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<A, B>\n"
        "  public virtual method Foo(x: Dictionary<A, B>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<P, Q>: G<Q, P>\n"
        "  public override method Foo(x: Dictionary<Q, P>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int, string> := new D<string, int>();\n"
        "    Console.WriteLine(b.Foo(new Dictionary<int, string>()).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `D<T>: G<Array<T>>`: the derived parameter shares the base parameter name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_same_name_type_parameter_wrapped_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method Foo(x: Array<T>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<T>: G<Array<T>>\n"
        "  public override method Foo(x: Array<Array<T>>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<Array<int>> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Array<Array<int>>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §12.1 / §14.1 / §27.8 v0.338: `Array<Array<T?>>` under `D<U>: G<U>`.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           method_nested_nullable_array_of_array_12_1_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class G<T>\n"
        "  public virtual method Foo(x: Array<Array<T?>>): int return 1; end Foo;\n"
        "end G;\n"
        "public class D<U>: G<U>\n"
        "  public override method Foo(x: Array<Array<U?>>): int return 2; end Foo;\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var b: G<int> := new D<int>();\n"
        "    Console.WriteLine(b.Foo(new Array<Array<int?>>(1)).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): C<T>: IG<T> with Foo(int) declared before Foo(T); call via IG<string> reaches Foo(T); the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_impl_overloads_int_before_t_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T): int;\n"
        "end IG;\n"
        "public class C<T>: IG<T>\n"
        "  public method Foo(x: int): int return 1; end Foo;\n"
        "  public method Foo(x: T): int return 2; end Foo;\n"
        "end C;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var i: IG<string> := new C<string>();\n"
        "    Console.WriteLine(i.Foo(\"s\").ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): same with Foo(string) before Foo(T); the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_impl_overloads_string_before_t_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T): int;\n"
        "end IG;\n"
        "public class C<T>: IG<T>\n"
        "  public method Foo(x: string): int return 1; end Foo;\n"
        "  public method Foo(x: T): int return 2; end Foo;\n"
        "end C;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var i: IG<int> := new C<int>();\n"
        "    Console.WriteLine(i.Foo(3).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): Foo(T?) next to Foo(string); the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_nullable_t_overload_before_string_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T?): int;\n"
        "end IG;\n"
        "public class C<T>: IG<T>\n"
        "  public method Foo(x: string): int return 1; end Foo;\n"
        "  public method Foo(x: T?): int return 2; end Foo;\n"
        "end C;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var i: IG<int> := new C<int>();\n"
        "    Console.WriteLine(i.Foo(3).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): G<T> virtual Foo(string) + Foo(T) through IG<int>, on D<int> and G<int>; the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_base_virtual_overloads_via_interface_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T): int;\n"
        "end IG;\n"
        "public class G<T>: IG<T>\n"
        "  public virtual method Foo(x: string): int return 1; end Foo;\n"
        "  public virtual method Foo(x: T): int return 2; end Foo;\n"
        "end G;\n"
        "public class D<T>: G<T>\n"
        "end D;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var i: IG<int> := new D<int>();\n"
        "    var j: IG<int> := new G<int>();\n"
        "    Console.WriteLine(i.Foo(3).ToString()+\" \"+j.Foo(3).ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2 2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): IConv<A,B> Conv(int,int) + Conv(A,B); the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_generic_pair_interface_overloads_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IConv<A,B>\n"
        "  method Conv(a: A, b: B): int;\n"
        "end IConv;\n"
        "public class C<A,B>: IConv<A,B>\n"
        "  public method Conv(a: int, b: int): int return 1; end Conv;\n"
        "  public method Conv(a: A, b: B): int return 2; end Conv;\n"
        "end C;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var a: IConv<string,string> := new C<string,string>();\n"
        "    Console.WriteLine(a.Conv(\"s\",\"t\").ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}

// §22.6 / §11.2 / §14 v0.338 (review of #6075): Foo(Object) declared before Foo(T); the interface-typed call reaches the
// overload sema resolved. The witness match must read the interface's `T` through the implementing
// chain, not by name.
BEZEL_TEST(codegen_generic_base_override_slot_6075,
           interface_witness_object_overload_before_t_22_6_v0338) {
    auto r = runAot(
        "module Al;\n"
        "public class Item end Item;\n"
        "public class Box<T> end Box;\n"
        "public interface IG<T>\n"
        "  method Foo(x: T): int;\n"
        "end IG;\n"
        "public class C<T>: IG<T>\n"
        "  public method Foo(x: Object): int return 1; end Foo;\n"
        "  public method Foo(x: T): int return 2; end Foo;\n"
        "end C;\n"
        "public class X\n"
        "  public static method Main()\n"
        "    var a: IG<string> := new C<string>();\n"
        "    Console.WriteLine(a.Foo(\"s\").ToString());\n"
        "  end Main;\n"
        "end X;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText == "2\n");
}
