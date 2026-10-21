// Spec-derived regression test for issue #6782 (the fix-forward of its first
// cut): §7.4 (v0.340) narrowed nullables at built-in parameters run and print
// the right value.
//
// §7.4: after `if x <> null`, a read of `x` is non-null, and "`readonly`
// fields narrow". The first cut of #6782 checked every nullable argument at a
// tabled built-in position as a `string` assignment. A narrowed `Foo?` at
// `StringBuilder.Append(value: Object)` and a narrowed `int?` at
// `StringBuilder(capacity: int)` (stdlib §3.1) were then falsely rejected with
// a §5.1 error, and a guarded `readonly` field at `int.Parse(s: string)`
// (stdlib §9.1) or an exception's `message` (§31.4) with a §7 error. Each row
// is a program that compiled and printed the value below before the first cut;
// exact stdout is the oracle.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

// stdlib §3.1 `Append(value: Object)`: appends `o.ToString()`, the qualified
// class name `M.Foo` (§8.5), five characters. Review probe a19.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a19_append_narrowed_object_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class Foo end Foo;
public class Program
  public static method Show(c: bool): int
    var sb := new StringBuilder();
    var o: Foo? := (if c then new Foo() else null);
    if o <> null then sb.Append(o); end;
    Console.WriteLine(sb.Length);
    return 0;
  end Show;
  public static method Main(): int
    return Show(true);
  end Main;
end Program;
)"), "5");
}

// stdlib §3.1 `StringBuilder(capacity: int)`. Review probe a48.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a48_ctor_capacity_narrowed_int_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class Program
  public static method Show(ni: int?): int
    if ni <> null then
      var sb := new StringBuilder(ni);
      sb.Append("x");
      Console.WriteLine(sb.ToString());
    end;
    return 0;
  end Show;
  public static method Main(): int
    return Show(16);
  end Main;
end Program;
)"), "x");
}

// §7.4 readonly instance field read through a local, at `int.Parse(s:
// string)`. Review probe a20.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a20_readonly_field_parse_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class H
  public readonly F: string?;
  public constructor(f: string?) F := f; end constructor;
end H;
public class Program
  public static method Show(s: string?): int
    var h := new H(s);
    if h.F <> null then Console.WriteLine(int.Parse(h.F)); end;
    return 0;
  end Show;
  public static method Main(): int
    return Show("5");
  end Main;
end Program;
)"), "5");
}

// The same field read through `this`. Review probe a26.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a26_this_readonly_field_parse_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class H
  public readonly F: string?;
  public constructor(f: string?) F := f; end constructor;
  public method P(): int
    if this.F <> null then return int.Parse(this.F); end;
    return -1;
  end P;
end H;
public class Program
  public static method Main(): int
    var h := new H("5");
    Console.WriteLine(h.P());
    return 0;
  end Main;
end Program;
)"), "5");
}

// §7.4 "instance or static" readonly: a `static readonly` field. Review probe
// a42.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a42_static_readonly_field_parse_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class G
  public static readonly S: string? := "12";
end G;
public class Program
  public static method Main(): int
    if G.S <> null then Console.WriteLine(int.Parse(G.S)); end;
    return 0;
  end Main;
end Program;
)"), "12");
}

// §31.4 `Exception(message: string)` with a guarded readonly field. Review
// probe a29.
BEZEL_TEST(builtin_narrowed_nullable_arg_6782,
           a29_readonly_field_exception_message_spec_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(R"(
module M;
public class H
  public readonly F: string?;
  public constructor(f: string?) F := f; end constructor;
end H;
public class Program
  public static method Show(s: string?): int
    var h := new H(s);
    if h.F <> null then
      using var ex := new Exception(h.F);
      Console.WriteLine(ex.Message);
    end;
    return 0;
  end Show;
  public static method Main(): int
    return Show("5");
  end Main;
end Program;
)"), "5");
}
