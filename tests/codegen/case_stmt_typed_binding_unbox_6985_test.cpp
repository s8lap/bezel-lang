// Spec-derived regression tests for fix #6985 — a case STATEMENT with a typed
// binding `var n: T` over an `Object`/`Object?` (or nullable value) scrutinee
// binds the UNBOXED / UNWRAPPED value, exactly as the case EXPRESSION does.
//
// Authority: spec §19.3 (typed binding is a type-test + binding; the bound name
// has type T), §19.5 (typed bindings never match null), §8.6 (unboxing),
// v0.340. Pre-fix `lowerCaseStmt` stored the raw box pointer, so `n + 1`
// failed IR verification (`add ptr, i32`). Expression-form twin: #746 / #738.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// runAot, not runAotChecked: a `case` over an Object local holding a §8.6 box
// leaks the box independently of the arm (#7005, MEASURED with a `_` arm only),
// so the leak oracle would fail on unrelated pre-existing behavior. Memory
// safety of the binding itself is covered by the ASan run in the lane handback.
std::string runOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran || r.exitCode != 0) return "<failed>";
    return r.stdoutText;
}
}  // namespace

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_int_obj) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_long) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 5000000000L;
    case o of
      var n: long => Console.WriteLine((n + 1L).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "5000000001\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_double) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 1.5;
    case o of
      var n: double => Console.WriteLine((n * 2.0).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "3\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_bool) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := true;
    case o of
      var n: bool => Console.WriteLine((if n then "yes" else "no")),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "yes\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_char) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 'a';
    case o of
      var n: char => Console.WriteLine(n.ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "a\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_string) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := "hi";
    case o of
      var n: string => Console.WriteLine(n + "!"),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "hi!\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_record) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := new Pt(3, 4);
    case o of
      var p: Pt => Console.WriteLine((p.X + p.Y).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "7\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_class) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := new Cl(9);
    case o of
      var c: Cl => Console.WriteLine(c.V.ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "9\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nonmatch_str) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := "s";
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nonmatch_long) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 5L;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nonmatch_rec) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 5;
    case o of
      var p: Pt => Console.WriteLine(p.X.ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_null_obj) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := null;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nullable_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: int? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nullable_int_null) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: int? := null;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_nullable_long) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: long? := 7L;
    case o of
      var n: long => Console.WriteLine((n + 1L).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "8\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_guard_true) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 41;
    case o of
      var n: int when n > 10 => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_guard_false) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 41;
    case o of
      var n: int when n > 100 => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_multi_arm) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 41;
    case o of
      var s: string => Console.WriteLine("str"),
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_multi_arm_second) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := "q";
    case o of
      var n: int => Console.WriteLine("int"),
      var s: string => Console.WriteLine(s + s),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "qq\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_guard_fall_to_next) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 41;
    case o of
      var n: int when n > 100 => Console.WriteLine("big"),
      var m: int => Console.WriteLine((m - 1).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "40\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_rec_guard) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := new Pt(3, 4);
    case o of
      var p: Pt when p.X = 3 => Console.WriteLine((p.Y * 10).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "40\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: Object? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "42\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_string) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: Object? := "hi";
    case o of
      var n: string => Console.WriteLine(n + "!"),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "hi!\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_record) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: Object? := new Pt(3, 4);
    case o of
      var p: Pt => Console.WriteLine((p.X + p.Y).ToString()),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "7\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_nonmatch_str) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: Object? := "s";
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "other\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_nullable_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: int? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "42\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, iter_stmt_guard_true) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Gen(): IEnumerable<int>
    var o: Object? := 41;
    case o of
      var n: int when n > 10 => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    yield return 1;
  end Gen;
  public static method Main()
    for var i in Program.Gen() do Console.WriteLine("i" + i.ToString()); end;
  end Main;
end Program;
)BZL"), "42\ni1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: Object? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "42\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_string) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: Object? := "hi";
    case o of
      var n: string => Console.WriteLine(n + "!"),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "hi!\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_record) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: Object? := new Pt(3, 4);
    case o of
      var p: Pt => Console.WriteLine((p.X + p.Y).ToString()),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "7\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_nonmatch_str) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: Object? := "s";
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "other\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_nullable_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: int? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "42\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, async_stmt_guard_true) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static async method Work(): Task<int>
    var o: Object? := 41;
    case o of
      var n: int when n > 10 => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    return 1;
  end Work;
  public static async method Main(): Task<int>
    var r := await Program.Work();
    Console.WriteLine("a" + r.ToString());
    return 0;
  end Main;
end Program;
)BZL"), "42\na1\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, loop100_stmt_int) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var k := 0;
    while k < 100 do
    k := k + 1;
    var o: Object? := 41;
    case o of
      var n: int => Console.WriteLine((n + 1).ToString()),
      _ => Console.WriteLine("other");
    end;
    end;
  end Main;
end Program;
)BZL"), "42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n42\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, loop100_stmt_string) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var k := 0;
    while k < 100 do
    k := k + 1;
    var o: Object? := "hi";
    case o of
      var n: string => Console.WriteLine(n + "!"),
      _ => Console.WriteLine("other");
    end;
    end;
  end Main;
end Program;
)BZL"), "hi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\nhi!\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, loop100_stmt_record) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var k := 0;
    while k < 100 do
    k := k + 1;
    var o: Object? := new Pt(3, 4);
    case o of
      var p: Pt => Console.WriteLine((p.X + p.Y).ToString()),
      _ => Console.WriteLine("other");
    end;
    end;
  end Main;
end Program;
)BZL"), "7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n7\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, loop100_stmt_class) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var k := 0;
    while k < 100 do
    k := k + 1;
    var o: Object? := new Cl(9);
    case o of
      var c: Cl => Console.WriteLine(c.V.ToString()),
      _ => Console.WriteLine("other");
    end;
    end;
  end Main;
end Program;
)BZL"), "9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n9\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_decimal) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 1.25m;
    case o of
      var n: decimal => Console.WriteLine((n * 2m).ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "2.5\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, stmt_decimal_nonmatch) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
class Cl
  public readonly property V: int;
  public constructor(v: int)  V := v;  end constructor;
end Cl;
public class Program
  public static method Main()
    var o: Object? := 5;
    case o of
      var n: decimal => Console.WriteLine(n.ToString()),
      _ => Console.WriteLine("other");
    end;
  end Main;
end Program;
)BZL"), "other\n");
}

BEZEL_TEST(case_stmt_typed_binding_unbox_6985, object_parameter_record_long_double_decimal) {
    BEZEL_EXPECT_EQ(runOut(R"BZL(
module P;
record Pt(X: int, Y: int);
public class Program
  public static method F(o: Object): string
    var r := "";
    case o of
      var n: Pt => r := "p" + (n.X + n.Y).ToString(),
      var n: long => r := "l" + (n + 1L).ToString(),
      var n: double => r := "d" + (n * 2.0).ToString(),
      var n: decimal => r := "m" + (n * 2m).ToString(),
      _ => r := "?"
    end;
    return r;
  end F;
  public static method Main()
    Console.WriteLine(Program.F(new Pt(3, 4)));
    Console.WriteLine(Program.F(7L));
    Console.WriteLine(Program.F(1.5));
    Console.WriteLine(Program.F(1.25m));
    Console.WriteLine(Program.F("x"));
  end Main;
end Program;
)BZL"), "p7\nl8\nd3\nm2.5\n?\n");
}
