// Codegen -- stdlib section 5 *Ordering* (v0.357), stdlib section 4.4 *Sorting
// contract* (v0.357), issue 8384: `OrderBy` / `OrderByDescending` / `ThenBy` /
// `ThenByDescending` ranked only an `int` key.
//
// stdlib section 5 declares all four as
//
//     public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>
//         where TKey: IComparable<TKey>;
//
// so the order is the key type's own `IComparable<TKey>.CompareTo`, for every
// key type that has one. The lowering called the key function through a
// `(T) -> int` signature whatever it returned: a `string` or `double` key left
// the INPUT order, silently (exit 0); a `long` / `uint` / `ulong` / `float` /
// `DateTime` / `TimeSpan` / `Guid` / record / class key gave a wrong order; and a
// `decimal` key (a struct result read as an `i32`) SEGFAULTED. `ThenBy` kept at
// most two keys, so a third level crashed or was dropped.
//
// stdlib section 4.4 *Sorting contract* (1): "`OrderBy`, `OrderByDescending`,
// `ThenBy`, and `ThenByDescending` (section 5) are stable -- equal keys retain
// source order (`ThenBy`'s compositional semantics require it)."
//
// What the rows pin:
//   * the issue's two repros, verbatim;
//   * key type x operator x {lambda, typed lambda, method reference} x {Array,
//     LinkedList, IEnumerable parameter, a Where chain, ToArray} for every
//     primitive key type stdlib section 9 declares `IComparable` on;
//   * the built-in value records (`DateTime`, `TimeSpan`, `Guid`) and a user
//     enum / record / class that implements `IComparable<Self>`;
//   * `ThenBy` chains of two, three and four keys of mixed types;
//   * stability in both directions, an empty and a one-element source;
//   * an `OrderBy` of an already ordered sequence (ties keep the first order);
//   * a key typed by a type parameter, at each instantiation;
//   * the ownership of a `string` key (each key is evaluated once and released
//     once: no leak, and no over-release of the element's own string);
//   * the rejection of a key with no `IComparable<TKey>` order (`bool`, a
//     nullable, a record with no `CompareTo`), each beside its ADMITTING TWIN.
//
// Every accepted row goes through runAotChecked, so the internal-degrade
// warnings are fatal here and a leak fails the row.

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void expectOut(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

void expectNoLeak(const JitResult& r, const std::string& out) {
    expectOut(r, out);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const std::string text = diagnosticText(r);
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs << " allocation(s) / "
                  << leakedBytes << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(sawSummary ? leakedAllocs : 0ull, 0ull);
    BEZEL_EXPECT_EQ(sawSummary ? leakedBytes : 0ull, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// The key-type matrix: ONE element type `ty`, ordered by ITSELF as the key, by
// every operator of the family, through every callback form and over every
// receiver form. `Id` is the method reference; the two `ShowParam*` methods are
// the `IEnumerable<T>` parameter receivers.
std::string keyMatrixProgram(const std::string& ty, const std::string& elems) {
    const std::string loop =
        " do Console.Write($\"{e} \"); end;\n    Console.WriteLine();\n";
    auto row = [&](const std::string& label, const std::string& query) {
        return "    Console.Write(\"" + label + ": \");\n    for var e in " + query +
               loop;
    };
    std::string s = "module P;\npublic class Program\n";
    s += "  static method Id(v: " + ty + "): " + ty + " => v;\n";
    s += "  static method ShowParam(xs: IEnumerable<" + ty + ">)\n";
    s += "    for var e in xs.OrderBy(lambda(e) => e)" + loop + "  end ShowParam;\n";
    s += "  static method ShowParamDesc(xs: IEnumerable<" + ty + ">)\n";
    s += "    for var e in xs.OrderByDescending(Id)" + loop + "  end ShowParamDesc;\n";
    s += "  public static method Main()\n";
    s += "    var xs: Array<" + ty + "> := [" + elems + "];\n";
    s += row("asc-lambda", "xs.OrderBy(lambda(e) => e)");
    s += row("asc-typed", "xs.OrderBy(lambda(e: " + ty + "): " + ty + " => e)");
    s += row("desc-lambda", "xs.OrderByDescending(lambda(e) => e)");
    s += row("asc-methodref", "xs.OrderBy(Id)");
    s += row("desc-methodref", "xs.OrderByDescending(Id)");
    s += row("then-asc", "xs.OrderBy(lambda(e) => 0).ThenBy(lambda(e) => e)");
    s += row("then-desc-methodref", "xs.OrderBy(lambda(e) => 0).ThenByDescending(Id)");
    s += "    using var l := new LinkedList<" + ty + ">();\n";
    s += "    for var e in xs do l.AddLast(e); end;\n";
    s += row("list-asc", "l.OrderBy(lambda(e) => e)");
    s += row("list-desc-methodref", "l.OrderByDescending(Id)");
    s += row("list-then-desc",
             "l.OrderByDescending(lambda(e) => 0).ThenByDescending(lambda(e) => e)");
    s += "    Console.Write(\"param-asc: \");\n    ShowParam(xs);\n";
    s += "    Console.Write(\"param-desc-methodref: \");\n    ShowParamDesc(xs);\n";
    s += row("chain-asc", "xs.Where(lambda(e) => true).OrderBy(lambda(e) => e)");
    s += "    Console.Write(\"toarray-first: \");\n";
    s += "    using var arr := xs.OrderBy(lambda(e) => e).ToArray();\n";
    s += "    Console.WriteLine($\"{arr[0]}\");\n";
    s += "  end Main;\nend Program;\n";
    return s;
}

// What `keyMatrixProgram` prints when every row is in the key's own order:
// `asc` / `desc` are the ascending / descending renderings, `first` the least.
std::string keyMatrixOut(const std::string& asc, const std::string& desc,
                         const std::string& first) {
    std::string s;
    s += "asc-lambda: " + asc + " \n";
    s += "asc-typed: " + asc + " \n";
    s += "desc-lambda: " + desc + " \n";
    s += "asc-methodref: " + asc + " \n";
    s += "desc-methodref: " + desc + " \n";
    s += "then-asc: " + asc + " \n";
    s += "then-desc-methodref: " + desc + " \n";
    s += "list-asc: " + asc + " \n";
    s += "list-desc-methodref: " + desc + " \n";
    s += "list-then-desc: " + desc + " \n";
    s += "param-asc: " + asc + " \n";
    s += "param-desc-methodref: " + desc + " \n";
    s += "chain-asc: " + asc + " \n";
    s += "toarray-first: " + first + "\n";
    return s;
}

void expectKeyMatrix(const std::string& ty, const std::string& elems,
                     const std::string& asc, const std::string& desc,
                     const std::string& first) {
    expectNoLeak(runAotChecked(keyMatrixProgram(ty, elems)),
                 keyMatrixOut(asc, desc, first));
}

// A rejected key: the program does not compile, and the diagnostic is THE
// stdlib-section-5 one (not a crash, not an unrelated message).
void expectKeyRejected(const JitResult& r, const char* op) {
    BEZEL_EXPECT_FALSE(r.ran);
    const std::string needle =
        std::string("`.") + op + "` (stdlib \xC2\xA7""5) requires its key type to "
        "implement `IComparable<TKey>`";
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, needle.c_str()));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "not yet implemented"));
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repros, verbatim.
// ---------------------------------------------------------------------------

// THE ISSUE'S REPRO 1, verbatim. The `string` and `double` rows printed the
// input order (`b a c` / `1 6 0.25`); the `int` row is the issue's own control.
BEZEL_TEST(orderby_key_types_8384, issue_repro_1_string_and_double_keys_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<string> := ["b", "a", "c"];
    for var e in xs.OrderBy(lambda(e: string): string => e) do Console.Write($"{e} "); end;
    Console.WriteLine();
    var ds: Array<double> := [1.0, 6.0, 0.25];
    for var d in ds.OrderBy(lambda(d: double): double => d) do Console.Write($"{d} "); end;
    Console.WriteLine();
    var ns: Array<int> := [3, 1, 2];
    for var n in ns.OrderBy(lambda(n) => n) do Console.Write($"{n} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "a b c \n0.25 1 6 \n1 2 3 \n");
}

// THE ISSUE'S REPRO 2, verbatim: `OrderBy` over `Array<decimal>` segfaulted
// (exit 139) before printing anything.
BEZEL_TEST(orderby_key_types_8384, issue_repro_2_decimal_key_does_not_crash_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var ms: Array<decimal> := [3.5m, 1.25m, 2.0m];
    var n := 0;
    for var m in ms.OrderBy(lambda(m) => m) do n++; end;
    Console.WriteLine(n);
  end Main;
end Program;
)BZL"),
                 "3\n");
}

// ---------------------------------------------------------------------------
// Key type x operator x callback form x receiver form (stdlib section 5; the
// conformances are stdlib section 9's `IComparable<…>` declarations).
// ---------------------------------------------------------------------------

BEZEL_TEST(orderby_key_types_8384, key_matrix_sbyte_stdlib_5_9_1) {
    expectKeyMatrix("sbyte", "3, 1, -100, 2, 100", "-100 1 2 3 100", "100 3 2 1 -100",
                    "-100");
}

// A CONTROL, green before the fix as well: a `byte` key is narrower than the
// 32-bit signed key every key used to be ranked as, and it zero-extends into
// it, so 200 and 130 were never misranked. What the row pins is that the
// per-type comparator the fix introduced keeps the UNSIGNED order at this
// width (an 8-bit SIGNED compare would rank 200 and 130 below 1), in every
// operator and callback form of the matrix.
BEZEL_TEST(orderby_key_types_8384, key_matrix_byte_stdlib_5_9_1) {
    expectKeyMatrix("byte", "3, 1, 200, 2, 130", "1 2 3 130 200", "200 130 3 2 1", "1");
}

BEZEL_TEST(orderby_key_types_8384, key_matrix_short_stdlib_5_9_1) {
    expectKeyMatrix("short", "3, 1, -200, 2, 300", "-200 1 2 3 300", "300 3 2 1 -200",
                    "-200");
}

// A CONTROL like the `byte` row, and for the same reason: 60000 and 40000 fit
// the old 32-bit signed key. Pins the unsigned order at 16 bits (a 16-bit
// SIGNED compare would rank them below 1).
BEZEL_TEST(orderby_key_types_8384, key_matrix_ushort_stdlib_5_9_1) {
    expectKeyMatrix("ushort", "3, 1, 60000, 2, 40000", "1 2 3 40000 60000",
                    "60000 40000 3 2 1", "1");
}

// The control: an `int` key was always ranked.
BEZEL_TEST(orderby_key_types_8384, key_matrix_int_control_stdlib_5_9_1) {
    expectKeyMatrix("int", "3, 1, 2, -7, 100", "-7 1 2 3 100", "100 3 2 1 -7", "-7");
}

BEZEL_TEST(orderby_key_types_8384, key_matrix_uint_stdlib_5_9_1) {
    expectKeyMatrix("uint", "3, 1, 4000000000U, 2, 7", "1 2 3 7 4000000000",
                    "4000000000 7 3 2 1", "1");
}

// A 64-bit key was ranked by its low 32 bits: 4294967296 (low half 0) came
// before 1.
BEZEL_TEST(orderby_key_types_8384, key_matrix_long_stdlib_5_9_1) {
    expectKeyMatrix("long", "3000000000L, 1L, 2L, -7000000000L, 4294967296L",
                    "-7000000000 1 2 3000000000 4294967296",
                    "4294967296 3000000000 2 1 -7000000000", "-7000000000");
}

BEZEL_TEST(orderby_key_types_8384, key_matrix_ulong_stdlib_5_9_1) {
    expectKeyMatrix("ulong", "3, 1, 18000000000000000000UL, 2, 7",
                    "1 2 3 7 18000000000000000000", "18000000000000000000 7 3 2 1",
                    "1");
}

BEZEL_TEST(orderby_key_types_8384, key_matrix_float_stdlib_5_9_2) {
    expectKeyMatrix("float", "1.5f, 6.0f, 0.25f, -3.5f, 2.0f", "-3.5 0.25 1.5 2 6",
                    "6 2 1.5 0.25 -3.5", "-3.5");
}

// The issue's `double` key, in every form: all rows printed `1 6 0.25 -3.5 2`.
BEZEL_TEST(orderby_key_types_8384, key_matrix_double_stdlib_5_9_2) {
    expectKeyMatrix("double", "1.0, 6.0, 0.25, -3.5, 2.0", "-3.5 0.25 1 2 6",
                    "6 2 1 0.25 -3.5", "-3.5");
}

// The issue's `decimal` key, in every form: the first row segfaulted.
BEZEL_TEST(orderby_key_types_8384, key_matrix_decimal_stdlib_5_9_3) {
    expectKeyMatrix("decimal", "3.5m, 1.25m, 2.0m, -7.75m, 100.125m",
                    "-7.75 1.25 2 3.5 100.125", "100.125 3.5 2 1.25 -7.75", "-7.75");
}

// `char` orders by code point: 'Z' (U+005A) < 'a' < 'b' < 'c' < 'é' (U+00E9).
// A CONTROL, green before the fix as well (these code points fit the old 32-bit
// signed key): it pins that the `char` comparator keeps the code-point order.
BEZEL_TEST(orderby_key_types_8384, key_matrix_char_stdlib_5_9_5) {
    expectKeyMatrix("char", "'c', 'a', 'Z', 'b', '\xC3\xA9'", "Z a b c \xC3\xA9",
                    "\xC3\xA9 c b a Z", "Z");
}

// The issue's `string` key, in every form. stdlib section 9.6: `CompareTo` is
// ordinal, so the capital sorts first.
BEZEL_TEST(orderby_key_types_8384, key_matrix_string_stdlib_5_9_6) {
    expectKeyMatrix("string", "\"pear\", \"apple\", \"fig\", \"Banana\", \"cherry\"",
                    "Banana apple cherry fig pear", "pear fig cherry apple Banana",
                    "Banana");
}

// ---------------------------------------------------------------------------
// The built-in value records and user types that implement IComparable<Self>.
// ---------------------------------------------------------------------------

// stdlib section 2.5 / 2.6: `TimeSpan` and `Guid` declare `IComparable<Self>`.
// (`DateTime` is issue 7780's key; its rows are in orderby_datetime_key_7780.)
BEZEL_TEST(orderby_key_types_8384, timespan_and_guid_keys_stdlib_5_2_5_2_6) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var t: Array<TimeSpan> := [TimeSpan.FromSeconds(30.0), TimeSpan.FromDays(400.0), TimeSpan.FromSeconds(-5.0), TimeSpan.FromHours(1.0)];
    Console.Write("ts asc: ");
    for var d in t.OrderBy(lambda(x) => x) do Console.Write($"{d.TotalSeconds} "); end;
    Console.WriteLine();
    Console.Write("ts desc: ");
    for var d in t.OrderByDescending(lambda(x) => x) do Console.Write($"{d.TotalSeconds} "); end;
    Console.WriteLine();
    var g: Array<Guid> := [Guid.Parse("ffffffff-0000-0000-0000-000000000001"), Guid.Parse("00000000-0000-0000-0000-000000000002"), Guid.Parse("80000000-0000-0000-0000-000000000003"), Guid.Parse("00000000-0000-0000-0000-000000000001")];
    Console.Write("guid asc: ");
    for var d in g.OrderBy(lambda(x) => x) do Console.Write($"{d} "); end;
    Console.WriteLine();
    Console.Write("guid desc: ");
    for var d in g.OrderByDescending(lambda(x) => x) do Console.Write($"{d} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "ts asc: -5 30 3600 34560000 \n"
                 "ts desc: 34560000 3600 30 -5 \n"
                 "guid asc: 00000000-0000-0000-0000-000000000001 "
                 "00000000-0000-0000-0000-000000000002 "
                 "80000000-0000-0000-0000-000000000003 "
                 "ffffffff-0000-0000-0000-000000000001 \n"
                 "guid desc: ffffffff-0000-0000-0000-000000000001 "
                 "80000000-0000-0000-0000-000000000003 "
                 "00000000-0000-0000-0000-000000000002 "
                 "00000000-0000-0000-0000-000000000001 \n");
}

// spec section 14.2 *Interface conformance* rules 1, 2 and 4: a class, a record
// and an enum that list `IComparable<Self>` and declare `CompareTo` satisfy the
// bound, and the order is THAT `CompareTo` -- `Box` reverses it on purpose, so
// an order taken from anything else (the pointer, a field) is visible.
BEZEL_TEST(orderby_key_types_8384, enum_record_and_class_keys_use_their_compareto_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
enum Priority: IComparable<Priority>
  Low, Medium, High;

  public method CompareTo(other: Priority): int =>
    (if int(this) < int(other) then -1
     elif int(this) > int(other) then 1
     else 0);
end Priority;
record Ver(Major: int, Minor: int): IComparable<Ver>
  public method CompareTo(other: Ver): int =>
    (if Major <> other.Major then Major - other.Major else Minor - other.Minor);
end Ver;
class Box: IComparable<Box>
  public readonly property W: int;
  public constructor(w: int) W := w; end constructor;
  public method CompareTo(other: Box): int => other.W - W;
end Box;
record Item(Name: string, P: Priority, V: Ver, Id: int);
public class Program
  public static method Main()
    using var a := new Array<Item>();
    a.Add(new Item("x", Priority.High, new Ver(1, 2), 1));
    a.Add(new Item("y", Priority.Low, new Ver(1, 0), 2));
    a.Add(new Item("z", Priority.Medium, new Ver(0, 9), 3));
    a.Add(new Item("w", Priority.Low, new Ver(2, 0), 4));
    Console.Write("enum asc: ");
    for var i in a.OrderBy(lambda(i) => i.P) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
    Console.Write("enum desc: ");
    for var i in a.OrderByDescending(lambda(i) => i.P) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
    Console.Write("record asc: ");
    for var i in a.OrderBy(lambda(i) => i.V) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
    Console.Write("record desc: ");
    for var i in a.OrderByDescending(lambda(i) => i.V) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
    Console.Write("enum,record-desc: ");
    for var i in a.OrderBy(lambda(i) => i.P).ThenByDescending(lambda(i) => i.V) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
    using var b := new Array<Box>();
    var b1 := new Box(5); var b2 := new Box(9); var b3 := new Box(1);
    b.Add(b1); b.Add(b2); b.Add(b3);
    Console.Write("class asc (reverse CompareTo): ");
    for var x in b.OrderBy(lambda(x) => x) do Console.Write($"{x.W} "); end;
    Console.WriteLine();
    Console.Write("class desc: ");
    for var x in b.OrderByDescending(lambda(x) => x) do Console.Write($"{x.W} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "enum asc: 2 4 3 1 \n"
                 "enum desc: 1 3 2 4 \n"
                 "record asc: 3 2 1 4 \n"
                 "record desc: 4 1 2 3 \n"
                 "enum,record-desc: 4 2 3 1 \n"
                 "class asc (reverse CompareTo): 9 5 1 \n"
                 "class desc: 1 5 9 \n");
}

// ---------------------------------------------------------------------------
// ThenBy / ThenByDescending chains (stdlib section 5; section 4.4 *Sorting
// contract* (1): the family is stable, which is what makes a chain compose).
// ---------------------------------------------------------------------------

// Two, three and four keys of mixed types. The rows, by Id:
//   1 (b, 2.5, 1, 9.5)  2 (a, 2.5, 2, 1.5)  3 (b, 0.5, 1, 3.5)
//   4 (a, 7.5, 1, 1.5)  5 (b, 2.5, 1, 0.5)  6 (a, 2.5, 1, 1.5)
// A chain kept at most two keys: the third level was dropped or crashed.
BEZEL_TEST(orderby_key_types_8384, thenby_chains_of_two_three_and_four_mixed_keys_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
record Row(Name: string, Score: double, Qty: int, Price: decimal, Id: int);
public class Program
  static method Show(label: string, xs: IEnumerable<Row>)
    Console.Write(label);
    for var r in xs do Console.Write($"{r.Id} "); end;
    Console.WriteLine();
  end Show;
  static method NameOf(r: Row): string => r.Name;
  static method ScoreOf(r: Row): double => r.Score;
  public static method Main()
    using var a := new Array<Row>();
    a.Add(new Row("b", 2.5, 1, 9.5m, 1));
    a.Add(new Row("a", 2.5, 2, 1.5m, 2));
    a.Add(new Row("b", 0.5, 1, 3.5m, 3));
    a.Add(new Row("a", 7.5, 1, 1.5m, 4));
    a.Add(new Row("b", 2.5, 1, 0.5m, 5));
    a.Add(new Row("a", 2.5, 1, 1.5m, 6));
    Show("name: ", a.OrderBy(lambda(r) => r.Name));
    Show("name-desc: ", a.OrderByDescending(lambda(r) => r.Name));
    Show("score: ", a.OrderBy(lambda(r) => r.Score));
    Show("price-desc: ", a.OrderByDescending(lambda(r) => r.Price));
    Show("name,score: ", a.OrderBy(lambda(r) => r.Name).ThenBy(lambda(r) => r.Score));
    Show("name,score-desc: ", a.OrderBy(lambda(r) => r.Name).ThenByDescending(lambda(r) => r.Score));
    Show("name-desc,price: ", a.OrderByDescending(lambda(r) => r.Name).ThenBy(lambda(r) => r.Price));
    Show("qty,name: ", a.OrderBy(lambda(r) => r.Qty).ThenBy(lambda(r) => r.Name));
    Show("name,score,qty: ", a.OrderBy(lambda(r) => r.Name).ThenBy(lambda(r) => r.Score).ThenBy(lambda(r) => r.Qty));
    Show("name,score,qty-desc: ", a.OrderBy(lambda(r) => r.Name).ThenBy(lambda(r) => r.Score).ThenByDescending(lambda(r) => r.Qty));
    Show("qty,score,name,price-desc: ", a.OrderBy(lambda(r) => r.Qty).ThenBy(lambda(r) => r.Score).ThenBy(lambda(r) => r.Name).ThenByDescending(lambda(r) => r.Price));
    Show("mref name,score: ", a.OrderBy(NameOf).ThenBy(ScoreOf));
    Show("mref name-desc,score-desc: ", a.OrderByDescending(NameOf).ThenByDescending(ScoreOf));
    Show("int qty,id-desc: ", a.OrderBy(lambda(r) => r.Qty).ThenByDescending(lambda(r) => r.Id));
  end Main;
end Program;
)BZL"),
                 "name: 2 4 6 1 3 5 \n"
                 "name-desc: 1 3 5 2 4 6 \n"
                 "score: 3 1 2 5 6 4 \n"
                 "price-desc: 1 3 2 4 6 5 \n"
                 "name,score: 2 6 4 3 1 5 \n"
                 "name,score-desc: 4 2 6 1 5 3 \n"
                 "name-desc,price: 5 3 1 2 4 6 \n"
                 "qty,name: 4 6 1 3 5 2 \n"
                 "name,score,qty: 6 2 4 3 1 5 \n"
                 "name,score,qty-desc: 2 6 4 3 1 5 \n"
                 "qty,score,name,price-desc: 3 6 1 5 4 2 \n"
                 "mref name,score: 2 6 4 3 1 5 \n"
                 "mref name-desc,score-desc: 1 5 3 4 2 6 \n"
                 "int qty,id-desc: 6 5 4 3 1 2 \n");
}

// stdlib section 4.4 *Sorting contract* (1): equal keys retain source order, in
// BOTH directions (a descending sort that reversed an ascending one would
// reverse the ties). 40 rows over three names and four scores; an empty and a
// one-element source are well-formed.
BEZEL_TEST(orderby_key_types_8384, order_is_stable_in_both_directions_stdlib_4_4_sorting_contract) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
record Row(Name: string, Score: int, Id: int);

public class Program
  public static method Main()
    using var rows := new Array<Row>();
    var i: int := 0;
    while i < 40 do
      var nm: string := "b";
      if i mod 3 = 0 then nm := "c"; end;
      if i mod 3 = 1 then nm := "a"; end;
      rows.Add(new Row(nm, i mod 4, i));
      i := i + 1;
    end;
    for var r in rows.OrderBy(lambda(r) => r.Name) do Console.Write($"{r.Id} "); end;
    Console.WriteLine("");
    for var r in rows.OrderByDescending(lambda(r) => r.Name) do Console.Write($"{r.Id} "); end;
    Console.WriteLine("");
    for var r in rows.OrderBy(lambda(r) => r.Score).ThenByDescending(lambda(r) => r.Name) do Console.Write($"{r.Id} "); end;
    Console.WriteLine("");
    using var empty := new Array<string>();
    Console.WriteLine(empty.OrderBy(lambda(x) => x).Count());
    var one: Array<string> := ["solo"];
    for var e in one.OrderBy(lambda(x) => x).ThenBy(lambda(x) => x.Length) do Console.Write($"{e} "); end;
    Console.WriteLine("");
  end Main;
end Program;
)BZL"),
                 // name asc: the `a` rows (i mod 3 = 1), then `b` (= 2), then `c` (= 0),
                 // each group in source order.
                 "1 4 7 10 13 16 19 22 25 28 31 34 37 "
                 "2 5 8 11 14 17 20 23 26 29 32 35 38 "
                 "0 3 6 9 12 15 18 21 24 27 30 33 36 39 \n"
                 // name desc: `c`, `b`, `a` -- each group STILL in source order.
                 "0 3 6 9 12 15 18 21 24 27 30 33 36 39 "
                 "2 5 8 11 14 17 20 23 26 29 32 35 38 "
                 "1 4 7 10 13 16 19 22 25 28 31 34 37 \n"
                 // score asc, then name desc (c, b, a), ties in source order.
                 "0 12 24 36 8 20 32 4 16 28 "
                 "9 21 33 5 17 29 1 13 25 37 "
                 "6 18 30 2 14 26 38 10 22 34 "
                 "3 15 27 39 11 23 35 7 19 31 \n"
                 "0\n"
                 "solo \n");
}

// stdlib section 4.4 *Sorting contract* (1), for a sort OF a sorted sequence:
// `xs.OrderBy(a).OrderBy(b)` starts a NEW order whose ties keep the order they
// arrive in -- the first sort's -- not the original source order. (A `ThenBy`
// chain may sort once from its `OrderBy`'s input; a fresh `OrderBy` may not
// look through the ordered sequence it is given.) The rows, by Id:
//   1 (A 2, B 0, x)  2 (A 1, B 0, y)  3 (A 2, B 1, x)  4 (A 1, B 1, y)
// so the source order is `1 2 3 4` and every re-sorted row but the control
// differs from it.
BEZEL_TEST(orderby_key_types_8384, orderby_of_an_ordered_sequence_keeps_its_order_among_ties_stdlib_4_4) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
record Row(A: int, B: int, Name: string, Id: int);
public class Program
  static method Show(label: string, xs: IEnumerable<Row>)
    Console.Write(label);
    for var r in xs do Console.Write($"{r.Id} "); end;
    Console.WriteLine();
  end Show;
  public static method Main()
    using var a := new Array<Row>();
    a.Add(new Row(2, 0, "x", 1));
    a.Add(new Row(1, 0, "y", 2));
    a.Add(new Row(2, 1, "x", 3));
    a.Add(new Row(1, 1, "y", 4));
    Show("a; b: ", a.OrderBy(lambda(r) => r.A).OrderBy(lambda(r) => r.B));
    Show("a-desc; b: ", a.OrderByDescending(lambda(r) => r.A).OrderBy(lambda(r) => r.B));
    Show("name-desc; b: ", a.OrderByDescending(lambda(r) => r.Name).OrderBy(lambda(r) => r.B));
    Show("name-desc; b-desc: ", a.OrderByDescending(lambda(r) => r.Name).OrderByDescending(lambda(r) => r.B));
    Show("a, id-desc; b: ", a.OrderBy(lambda(r) => r.A).ThenByDescending(lambda(r) => r.Id).OrderBy(lambda(r) => r.B));
    Show("id-desc; 0, b: ", a.OrderByDescending(lambda(r) => r.Id).OrderBy(lambda(r) => 0).ThenBy(lambda(r) => r.B));
    Show("name-desc; b, a: ", a.OrderByDescending(lambda(r) => r.Name).OrderBy(lambda(r) => r.B).ThenBy(lambda(r) => r.A));
    Show("name-desc; where; b: ", a.OrderByDescending(lambda(r) => r.Name).Where(lambda(r) => r.Id <> 3).OrderBy(lambda(r) => r.B));
    Show("name-desc; take 3; b: ", a.OrderByDescending(lambda(r) => r.Name).Take(3).OrderBy(lambda(r) => r.B));
  end Main;
end Program;
)BZL"),
                 // by A: 2 4 1 3; then by B, ties in THAT order.
                 "a; b: 2 1 4 3 \n"
                 // by A descending: 1 3 2 4; then by B (the control: equals the source order).
                 "a-desc; b: 1 2 3 4 \n"
                 // by Name descending: 2 4 1 3; then by B.
                 "name-desc; b: 2 1 4 3 \n"
                 "name-desc; b-desc: 4 3 2 1 \n"
                 // by A, Id descending: 4 2 3 1; then by B.
                 "a, id-desc; b: 2 1 4 3 \n"
                 // by Id descending: 4 3 2 1; then by (0, B).
                 "id-desc; 0, b: 2 1 4 3 \n"
                 // by Name descending: 2 4 1 3; then by (B, A): A breaks every tie.
                 "name-desc; b, a: 2 1 4 3 \n"
                 // 2 4 1 (3 filtered out / not taken); then by B.
                 "name-desc; where; b: 2 1 4 \n"
                 "name-desc; take 3; b: 2 1 4 \n");
}

// ---------------------------------------------------------------------------
// A key typed by a type parameter (spec section 14.2 *Interface conformance*
// rule 6): each instantiation ranks by the type the parameter is bound to.
// ---------------------------------------------------------------------------

BEZEL_TEST(orderby_key_types_8384, type_parameter_key_ranks_by_its_binding_spec_14_2) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Sorter<T> where T: IComparable<T>
  public method Show(xs: Array<T>)
    for var e in xs.OrderBy(lambda(x) => x) do Console.Write($"{e} "); end;
    Console.WriteLine("");
  end Show;
  public method ShowBy<K>(xs: Array<T>, key: (T) -> K) where K: IComparable<K>
    for var e in xs.OrderByDescending(key) do Console.Write($"{e} "); end;
    Console.WriteLine("");
  end ShowBy;
end Sorter;
public class Program
  static method ShowSorted<T>(xs: Array<T>) where T: IComparable<T>
    for var e in xs.OrderBy(lambda(x) => x) do Console.Write($"{e} "); end;
    Console.WriteLine("");
    for var e in xs.OrderByDescending(lambda(x) => x) do Console.Write($"{e} "); end;
    Console.WriteLine("");
  end ShowSorted;
  static method ByKey<T, K>(xs: Array<T>, key: (T) -> K) where K: IComparable<K>
    for var e in xs.OrderBy(key) do Console.Write($"{e} "); end;
    Console.WriteLine("");
    for var e in xs.OrderBy(key).ThenByDescending(key) do Console.Write($"{e} "); end;
    Console.WriteLine("");
  end ByKey;
  static method Outer<T>(xs: Array<T>) where T: IComparable<T>
    ByKey<T, T>(xs, lambda(y: T): T => y);
  end Outer;
  public static method Main()
    using var a := new Array<int>();
    a.Add(3); a.Add(1); a.Add(2);
    using var s := new Array<string>();
    s.Add("pear"); s.Add("apple"); s.Add("fig");
    using var d := new Array<double>();
    d.Add(2.5); d.Add(0.25); d.Add(1.0);
    using var l := new Array<long>();
    l.Add(4294967296L); l.Add(1L); l.Add(-7000000000L);
    using var m := new Array<decimal>();
    m.Add(2.5m); m.Add(0.25m); m.Add(1.0m);
    ShowSorted<int>(a);
    ShowSorted<string>(s);
    ShowSorted<double>(d);
    ShowSorted<long>(l);
    ShowSorted<decimal>(m);
    ByKey<string, int>(s, lambda(x) => x.Length);
    ByKey<int, double>(a, lambda(x) => 1.0 / double(x));
    Outer<double>(d);
    Outer<string>(s);
    using var ss := new Sorter<string>();
    ss.Show(s);
    ss.ShowBy<int>(s, lambda(x) => x.Length);
    using var sm := new Sorter<decimal>();
    sm.Show(m);
    sm.ShowBy<decimal>(m, lambda(x) => -x);
  end Main;
end Program;
)BZL"),
                 "1 2 3 \n3 2 1 \n"
                 "apple fig pear \npear fig apple \n"
                 "0.25 1 2.5 \n2.5 1 0.25 \n"
                 "-7000000000 1 4294967296 \n4294967296 1 -7000000000 \n"
                 "0.25 1 2.5 \n2.5 1 0.25 \n"
                 // ByKey<string, int> by length: fig (3), pear (4), apple (5).
                 "fig pear apple \nfig pear apple \n"
                 // ByKey<int, double> by 1/x: 3, 2, 1.
                 "3 2 1 \n3 2 1 \n"
                 // Outer<double> / Outer<string>: ByKey<T, T> by the element itself.
                 "0.25 1 2.5 \n0.25 1 2.5 \n"
                 "apple fig pear \napple fig pear \n"
                 // Sorter<string>: ascending, then descending by length.
                 "apple fig pear \napple pear fig \n"
                 // Sorter<decimal>: ascending, then descending by -x (= ascending).
                 "0.25 1 2.5 \n0.25 1 2.5 \n");
}

// ---------------------------------------------------------------------------
// Other key-function forms: a method reference on another class, a stdlib
// static (`Math.Abs`), a callback read from a field.
// ---------------------------------------------------------------------------

BEZEL_TEST(orderby_key_types_8384, method_reference_and_field_held_key_functions_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
class Util
  public static method Neg(v: int): int => 0 - v;
  public static method Name(v: int): string => (if v < 10 then "b" else "a") + v.ToString();
end Util;
class Holder
  private _key: (int) -> double;
  public constructor()
    _key := lambda(v: int): double => 1.0 / double(v);
  end constructor;
  public method ByField(xs: Array<int>)
    for var e in xs.OrderBy(_key) do Console.Write($"{e} "); end;
    Console.WriteLine();
  end ByField;
end Holder;
public class Program
  public static method Main()
    var xs: Array<int> := [3, 12, 1, 20, 7];
    for var e in xs.OrderBy(Util.Neg) do Console.Write($"{e} "); end;
    Console.WriteLine();
    for var e in xs.OrderBy(Util.Name) do Console.Write($"{e} "); end;
    Console.WriteLine();
    for var e in xs.OrderByDescending(Util.Name).ThenBy(Util.Neg) do Console.Write($"{e} "); end;
    Console.WriteLine();
    for var e in xs.OrderBy(Math.Abs) do Console.Write($"{e} "); end;
    Console.WriteLine();
    using var h := new Holder();
    h.ByField(xs);
  end Main;
end Program;
)BZL"),
                 "20 12 7 3 1 \n"
                 // Name: 3 -> "b3", 12 -> "a12", 1 -> "b1", 20 -> "a20", 7 -> "b7".
                 "12 20 1 3 7 \n"
                 "7 3 1 20 12 \n"
                 "1 3 7 12 20 \n"
                 // 1/x ascending.
                 "20 12 7 3 1 \n");
}

// ---------------------------------------------------------------------------
// The ownership of a `string` key (spec section 28.9: a string is
// runtime-managed). The sort evaluates each key once and releases it once.
// ---------------------------------------------------------------------------

// Every string-key form over HEAP strings, 200 rounds. A FRESH key (`s + "#"`)
// that is not released leaks one string per element per round; an IDENTITY key
// (`s`) that is released without having been retained frees the element's own
// string. The total is the sum of the lengths the loops see: per round, with
// `d` the digit count of the round number, five whole-sequence loops (17 + 4d
// each), two `apple…` lengths (5 + d each), 1 for the `break` loop, 6 for the
// two `Count()`s, and the `Where(Length > 4)` loop (17 for d = 1, where `fig0`
// is filtered out; 17 + 4d otherwise): 141 x 10 + 171 x 90 + 197 x 100.
BEZEL_TEST(orderby_key_types_8384, string_keys_are_released_exactly_once_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  static method Key(s: string): string => s + "!";
  static method Same(s: string): string => s;
  public static method Main()
    var total := 0;
    for var round in 0..<200 do
      using var a := new Array<string>();
      a.Add("pear" + round.ToString());
      a.Add("apple" + round.ToString());
      a.Add("fig" + round.ToString());
      a.Add("apple" + round.ToString());
      for var s in a.OrderBy(lambda(s) => s) do total := total + s.Length; end;
      for var s in a.OrderByDescending(lambda(s) => s + "#") do total := total + s.Length; end;
      for var s in a.OrderBy(Key).ThenByDescending(lambda(s) => s.ToUpper()) do total := total + s.Length; end;
      for var s in a.OrderByDescending(Same).ThenBy(Same) do total := total + s.Length; end;
      using var arr := a.OrderBy(lambda(s) => s).ThenBy(lambda(s) => s.Length).ToArray();
      total := total + arr[0].Length;
      var f := a.OrderBy(lambda(s) => s + "z").First();
      total := total + f.Length;
      for var s in a.OrderBy(lambda(s) => s + "w") do total := total + 1; break; end;
      total := total + a.OrderBy(lambda(s) => s).Count() + a.OrderBy(lambda(s) => s + "x").Take(2).Count();
      for var s in a.Where(lambda(s) => s.Length > 4).OrderByDescending(lambda(s) => s.Substring(1)) do total := total + s.Length; end;
      for var s in a.OrderBy(lambda(s) => s.Length).ThenBy(lambda(s) => s.Substring(0, 1)).ThenByDescending(lambda(s) => s + "!").ThenBy(lambda(s) => s) do total := total + s.Length; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
)BZL"),
                 "36500\n");
}

// A key typed `T`, with `T` bound to `string`, over heap strings: the identity
// lambda `lambda(x) => x` in a generic body returns the element's own string
// WITHOUT retaining it, so the sort must not release it. 200 rounds; each round
// counts 4 + 4 elements.
BEZEL_TEST(orderby_key_types_8384, type_parameter_string_key_is_not_over_released_spec_28_9) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  static method Sorted<T>(xs: Array<T>): int where T: IComparable<T>
    var n := 0;
    for var e in xs.OrderBy(lambda(x) => x) do n := n + 1; end;
    for var e in xs.OrderByDescending(lambda(x) => x).ThenBy(lambda(x) => x) do n := n + 1; end;
    return n;
  end Sorted;
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<string>();
      a.Add("pear" + round.ToString());
      a.Add("apple" + round.ToString());
      a.Add("fig" + round.ToString());
      a.Add("apple" + round.ToString());
      total := total + Sorted<string>(a);
      last := a[2];
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
                 "1600\nfig199\n");
}

// ---------------------------------------------------------------------------
// A key lambda that CAPTURES a local (spec section 27.5: the closure captures
// the variable, which stays readable in its own scope; section 28.9: a string
// reference is released exactly once).
//
// Releasing the sort's keys exposed a defect that is older than this fix and
// not specific to ordering (issue 8553): a lambda literal handed to a lazy
// stdlib section 5 operator takes over the reference of a `string` or closure
// local it captures, and frees it when the QUERY is disposed -- inside the
// local's scope. `a.Where(lambda(x) => cap.Length > 0)` followed by a read of
// `cap` is a use-after-free with or without this fix. An `OrderBy` whose key
// IS the captured string did not crash before only because the sort leaked
// every key, and the leaked references kept the string alive.
//
// Until 8553 is fixed the sort therefore does NOT release the keys of a key
// lambda that captures a `string` or function-typed local: such a query keeps
// exactly the accounting it had (a leak), instead of trading it for a
// use-after-free. The rows below pin both halves:
//   * the captured-local rows run under AddressSanitizer with the LEAK check
//     off (runAotCheckedLenient): the output is right and nothing is read
//     after it was freed. They are NOT leak-clean, and say so. When 8553 is
//     fixed, move them to expectNoLeak;
//   * the twin rows (a key lambda that captures nothing, a value-typed local,
//     or an object) stay on expectNoLeak: the guard is narrow, and their keys
//     are still released once each.
// ---------------------------------------------------------------------------

namespace {

// 200 rounds over a fresh `[3, 1, 2]`. `decls` declares the round's locals,
// `query` is iterated (a loop adds 3 + 1 + 2 to the total whatever the order
// is), and `after` runs once the query has been disposed: it is where the
// captured local is READ AGAIN.
std::string capturedKeyProgram(const std::string& decls, const std::string& query,
                               const std::string& after) {
    return R"BZL(
module P;
class Holder
  public readonly property Tag: string;
  public constructor(t: string) Tag := t; end constructor;
end Holder;
public class Program
  static method Pick(s: string, x: int): string => s;
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3); a.Add(1); a.Add(2);
      )BZL" + decls + R"BZL(
      for var x in )BZL" + query + R"BZL( do total := total + x; end;
      )BZL" + after + R"BZL(
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL";
}

// The local is read after the query: the output is right and AddressSanitizer
// sees no use-after-free / double free (leak check off: see the section note).
// A red row prints the harness's own verdict line, so "the program crashed"
// cannot be mistaken for "the program did not build": both leave `ran` false.
void expectCapturedLocalSurvives(const std::string& decls, const std::string& query,
                                 const std::string& after, const std::string& out) {
    const JitResult r = runAotCheckedLenient(capturedKeyProgram(decls, query, after));
    const std::string text = diagnosticText(r);
    if (!text.empty()) {
        const std::size_t at = text.find("ERROR: AddressSanitizer");
        std::cout << "    [ HARNESS ] " << text.substr(0, text.find('\n')) << "\n";
        if (at != std::string::npos)
            std::cout << "    [ SANITIZER ] "
                      << text.substr(at, text.find('\n', at) - at).substr(0, 96) << "\n";
    }
    BEZEL_EXPECT_TRUE(text.find("sanitizer fired") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    expectOut(r, out);
}

}  // namespace

// The non-author review's probe p14, verbatim: the key is the captured string
// local, a `ThenBy` follows, and the local is read after the loop. With every
// key released this was `SEGV ... free <- bz_string_release <- main`.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_key_is_readable_after_the_query_review_p14_spec_27_5) {
    expectOut(runAotCheckedLenient(R"BZL(
module P;
public class Program
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3); a.Add(1); a.Add(2);
      var cap := "cap" + round.ToString();
      for var x in a.OrderBy(lambda(x) => cap).ThenBy(lambda(x) => x) do total := total + x; end;
      last := cap;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
              "1200\ncap199\n");
}

// The review's probe p2_cap, verbatim: the same key over an array of records,
// the local read in a concatenation after the loop (a heap-use-after-free in
// `bz_string_concat` with every key released).
BEZEL_TEST(orderby_key_types_8384, captured_string_local_key_over_records_review_p2_cap_spec_27_5) {
    expectOut(runAotCheckedLenient(R"BZL(
module P;
record Row(Name: string, Id: int);
public class Program
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<Row>();
      a.Add(new Row("pear" + round.ToString(), 1));
      a.Add(new Row("apple" + round.ToString(), 2));
      a.Add(new Row("fig" + round.ToString(), 3));
      var cap := "cap" + round.ToString();
      for var r in a.OrderBy(lambda(r) => cap) do total := total + r.Id; end;
      last := cap + a[1].Name;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
              "1200\ncap199apple199\n");
}

// The review's probe p16b, verbatim: the captured local is one arm of a
// conditional key, under `OrderByDescending`, beside a `using` object.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_in_a_conditional_key_review_p16b_spec_27_5) {
    expectOut(runAotCheckedLenient(R"BZL(
module P;
class Holder
  public readonly property Tag: string;
  public constructor(t: string) Tag := t; end constructor;
end Holder;
public class Program
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3); a.Add(1); a.Add(2);
      using var h := new Holder("t" + round.ToString());
      var cap := "c" + round.ToString();
      for var x in a.OrderByDescending(lambda(x) => (if x > 1 then cap else "z")) do total := total + x; end;
      last := h.Tag + cap;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
              "1200\nt199c199\n");
}

// Both keys of a chain are the same captured local.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_as_orderby_and_thenby_key_spec_27_5) {
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString();",
        "a.OrderBy(lambda(x) => cap).ThenBy(lambda(x) => cap)",
        "last := cap;", "1200\nc199\n");
}

// An eager terminal (`ToArray`) and a filtered source.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_key_under_toarray_and_after_where_spec_27_5) {
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString();",
        "a.OrderBy(lambda(x) => cap).ToArray()",
        "last := cap;", "1200\nc199\n");
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString();",
        "a.Where(lambda(x) => x > 0).OrderByDescending(lambda(x) => cap)",
        "last := cap;", "1200\nc199\n");
}

// The captured local reaches the key through a BLOCK body, through a method
// that returns its argument (`Pick`), and as a `const`.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_key_block_body_method_alias_and_const_spec_27_5) {
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString();",
        "a.OrderBy(lambda(x: int): string => do return cap; end)",
        "last := cap;", "1200\nc199\n");
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString();",
        "a.OrderBy(lambda(x) => Pick(cap, x))",
        "last := cap;", "1200\nc199\n");
    expectCapturedLocalSurvives(
        "const cap := \"c\" + round.ToString();",
        "a.OrderBy(lambda(x) => cap)",
        "last := cap;", "1200\nc199\n");
}

// The query is held in a local, runs in an inner scope, or is one of two
// queries over the same captured local.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_key_query_local_inner_scope_and_two_queries_spec_27_5) {
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString(); var q := a.OrderBy(lambda(x) => cap);",
        "q", "last := cap;", "1200\nc199\n");
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString(); if round >= 0 then for var y in "
        "a.OrderBy(lambda(x) => cap) do total := total + y; end; end;",
        "a.OrderBy(lambda(x) => x)", "last := cap;", "2400\nc199\n");
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString(); for var y in a.OrderBy(lambda(x) => cap) "
        "do total := total + y; end;",
        "a.OrderByDescending(lambda(x) => cap)", "last := cap;", "2400\nc199\n");
}

// The key reaches the captured string through a captured CLOSURE local: the
// key lambda's env releases `f`, and `f`'s env releases `cap`.
BEZEL_TEST(orderby_key_types_8384, captured_string_local_reached_through_a_closure_local_key_spec_27_5) {
    expectCapturedLocalSurvives(
        "var cap := \"c\" + round.ToString(); var f := lambda(x: int): string => cap;",
        "a.OrderBy(lambda(x) => f(x))", "last := cap;", "1200\nc199\n");
}

// A captured string PARAMETER is retained at the capture, so it was never
// over-released; the row is a control for the key drop over a +1 key that
// aliases a caller's string, read again by the caller AND the callee. (Not
// leak-clean either: the capture cell of a parameter leaks 8 bytes, with or
// without the fix.) Per round: 3 + 1 + 2, plus the tag's length (4, 5 or 6).
BEZEL_TEST(orderby_key_types_8384, captured_string_parameter_key_is_readable_after_the_query_spec_27_5) {
    expectOut(runAotCheckedLenient(R"BZL(
module P;
public class Program
  static method Run(a: Array<int>, tag: string): int
    var total := 0;
    for var x in a.OrderBy(lambda(x) => tag).ThenBy(lambda(x) => x) do total := total + x; end;
    return total + tag.Length;
  end Run;
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<int>();
      a.Add(3); a.Add(1); a.Add(2);
      var tag := "tag" + round.ToString();
      total := total + Run(a, tag);
      last := tag;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
              "2290\ntag199\n");
}

// THE TWINS: the guard is narrow. A key lambda that captures only a value-typed
// local (and mints its key), one that reads a field of a captured `using`
// object, and one that captures nothing beside an unrelated string local each
// still have every key released: no leak at all, and the locals read fine.
BEZEL_TEST(orderby_key_types_8384, key_lambda_capturing_an_int_local_still_releases_its_keys_spec_28_9) {
    expectNoLeak(runAotChecked(capturedKeyProgram(
                     "var off := round + 1;",
                     "a.OrderBy(lambda(x) => \"k\" + (x + off).ToString())",
                     "last := off.ToString();")),
                 "1200\n200\n");
}

BEZEL_TEST(orderby_key_types_8384, field_key_of_a_captured_using_object_still_releases_its_keys_spec_28_9) {
    expectNoLeak(runAotChecked(capturedKeyProgram(
                     "using var h := new Holder(\"t\" + round.ToString());",
                     "a.OrderBy(lambda(x) => h.Tag).ThenByDescending(lambda(x) => h.Tag)",
                     "last := h.Tag;")),
                 "1200\nt199\n");
}

BEZEL_TEST(orderby_key_types_8384, capture_free_key_beside_a_string_local_still_releases_its_keys_spec_28_9) {
    expectNoLeak(runAotChecked(capturedKeyProgram(
                     "var cap := \"c\" + round.ToString();",
                     "a.OrderBy(lambda(x) => \"k\" + x.ToString())"
                     ".ThenByDescending(lambda(x) => x.ToString())",
                     "last := cap;")),
                 "1200\nc199\n");
}

// A FIELD of the element as the key, over records: ranked by the string's own
// ordinal order (`apple…` first), each key retained by the key function and
// released by the sort, the elements' own names untouched and read afterwards.
// (Leak check off: an `Array` of records leaks its elements with or without an
// ordering in the program -- not this fix's subject.)
BEZEL_TEST(orderby_key_types_8384, element_field_string_key_over_records_spec_28_9) {
    expectOut(runAotCheckedLenient(R"BZL(
module P;
record Row(Name: string, Id: int);
public class Program
  public static method Main()
    var total := 0;
    var last := "";
    for var round in 0..<200 do
      using var a := new Array<Row>();
      a.Add(new Row("pear" + round.ToString(), 1));
      a.Add(new Row("apple" + round.ToString(), 2));
      a.Add(new Row("fig" + round.ToString(), 3));
      for var r in a.OrderBy(lambda(r) => r.Name) do total := total + r.Id; end;
      for var r in a.OrderByDescending(lambda(r) => r.Name).ThenBy(lambda(r) => r.Name) do total := total + r.Id; end;
      using var first := a.OrderBy(lambda(r) => r.Name).ToArray();
      last := first[0].Name + a[1].Name;
    end;
    Console.WriteLine(total);
    Console.WriteLine(last);
  end Main;
end Program;
)BZL"),
              "2400\napple199apple199\n");
}

// ---------------------------------------------------------------------------
// A key with no IComparable<TKey> order is REJECTED (stdlib section 5: `where
// TKey: IComparable<TKey>`), each beside its admitting twin. These keys were
// accepted and "ordered" by whatever the `int` read of the key produced.
// ---------------------------------------------------------------------------

// stdlib section 9.4: `public record Boolean: IEquatable<bool>` -- no
// `IComparable<bool>`.
BEZEL_TEST(orderby_key_types_8384, bool_key_is_rejected_stdlib_5_9_4) {
    expectKeyRejected(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<bool> := [true, false, true];
    for var e in xs.OrderBy(lambda(e) => e) do Console.Write($"{e} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                      "OrderBy");
}

// The admitting twin: the same sequence by an `int` derived from the flag.
BEZEL_TEST(orderby_key_types_8384, bool_element_with_int_key_is_admitted_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<bool> := [true, false, true];
    for var e in xs.OrderBy(lambda(e) => (if e then 1 else 0)) do Console.Write($"{e} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "False True True \n");
}

// spec section 14.2: `where T: IFoo` admits "Non-nullable types conforming to
// `IFoo`" -- an `int?` key does not satisfy the bound.
BEZEL_TEST(orderby_key_types_8384, nullable_int_key_is_rejected_spec_14_2) {
    expectKeyRejected(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<int?> := [3, null, 1];
    var n := 0;
    for var e in xs.OrderByDescending(lambda(e) => e) do n++; end;
    Console.WriteLine(n);
  end Main;
end Program;
)BZL"),
                      "OrderByDescending");
}

// The admitting twin: the nullable collapsed to an `int` key.
BEZEL_TEST(orderby_key_types_8384, nullable_int_element_with_coalesced_key_is_admitted_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<int?> := [3, null, 1];
    for var e in xs.OrderByDescending(lambda(e) => e ?? -1) do Console.Write($"{e ?? -1} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "3 1 -1 \n");
}

BEZEL_TEST(orderby_key_types_8384, nullable_string_key_is_rejected_spec_14_2) {
    expectKeyRejected(runAotChecked(R"BZL(
module P;
public class Program
  public static method Main()
    var xs: Array<string?> := ["b", null, "a"];
    var n := 0;
    for var e in xs.OrderBy(lambda(e) => e) do n++; end;
    Console.WriteLine(n);
  end Main;
end Program;
)BZL"),
                      "OrderBy");
}

// spec section 14.2 *Interface conformance* rule 2: a record conforms when it
// lists the interface and declares the member. `Pt` does neither.
BEZEL_TEST(orderby_key_types_8384, record_key_without_compareto_is_rejected_spec_14_2) {
    expectKeyRejected(runAotChecked(R"BZL(
module P;
record Pt(X: int, Y: int);
public class Program
  public static method Main()
    using var a := new Array<Pt>();
    a.Add(new Pt(2, 1)); a.Add(new Pt(1, 2));
    for var p in a.OrderBy(lambda(p) => p) do Console.Write($"{p.X} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                      "OrderBy");
}

// The same bound on `ThenBy`, with its admitting twin in one program each.
BEZEL_TEST(orderby_key_types_8384, thenby_bool_key_is_rejected_stdlib_5) {
    expectKeyRejected(runAotChecked(R"BZL(
module P;
record Pt(X: int, Y: int);
public class Program
  public static method Main()
    using var a := new Array<Pt>();
    a.Add(new Pt(2, 1)); a.Add(new Pt(1, 2));
    for var p in a.OrderBy(lambda(p) => p.X).ThenBy(lambda(p) => p.Y > 1) do Console.Write($"{p.X} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                      "ThenBy");
}

BEZEL_TEST(orderby_key_types_8384, thenby_int_key_twin_is_admitted_stdlib_5) {
    expectNoLeak(runAotChecked(R"BZL(
module P;
record Pt(X: int, Y: int);
public class Program
  public static method Main()
    using var a := new Array<Pt>();
    a.Add(new Pt(2, 1)); a.Add(new Pt(1, 2)); a.Add(new Pt(1, 1));
    for var p in a.OrderBy(lambda(p) => p.X).ThenBy(lambda(p) => p.Y) do Console.Write($"{p.X}{p.Y} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL"),
                 "11 12 21 \n");
}

// spec section 14.2 *Interface conformance* rule 7 admits a choice that lists
// `IComparable<Self>` and declares `CompareTo`. Its dispatch is NOT lowered yet
// (issue 8462): the call is declined at compile time with a message that says
// so and does NOT claim the key is not comparable. It used to compile and
// segfault at run time.
BEZEL_TEST(orderby_key_types_8384, comparable_choice_key_is_declined_not_miscompiled_8462) {
    const JitResult r = runAotChecked(R"BZL(
module P;
choice Rank: IComparable<Rank>
  Low,
  Level(n: int),
  High;

  public method Weight(): int => (case this of Low => 0, Level(n) => n, High => 1000);
  public method CompareTo(other: Rank): int => Weight() - other.Weight();
end Rank;
record Item(R: Rank, Id: int);
public class Program
  public static method Main()
    using var a := new Array<Item>();
    a.Add(new Item(Rank.High, 1));
    a.Add(new Item(Rank.Level(5), 2));
    a.Add(new Item(Rank.Low, 3));
    a.Add(new Item(Rank.Level(2), 4));
    for var i in a.OrderBy(lambda(i) => i.R) do Console.Write($"{i.Id} "); end;
    Console.WriteLine();
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "over a key of type `Rank` is not supported yet (issue #8462)"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(
        r, "requires its key type to implement `IComparable<TKey>`"));
    BEZEL_EXPECT_FALSE(
        bezel::test::diagnosticMessageContains(r, "IR verification failed"));
}
