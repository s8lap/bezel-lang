// #5061 (§11.2 / §14.1 generic base substitution; §23 auto-property, v0.313):
// a BARE identifier (implicit `this.`) naming an auto-property inherited from
// a generic base `B<T>` must carry the substituted type (`class E: B<int>` ->
// `int`), like `this.Q` and the field arm do. Before the fix the read's type
// was the unsubstituted `T` (printed `?`), so `Q + Q` was rejected with
// "operator `+` is not defined for type `?`".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string prog(const std::string& t, const std::string& lit,
                 const std::string& stmt) {
    return "module M5061; class B<T> public property Q: T; end B; "
           "class E: B<" + t + "> "
           "  public method Set(v: " + t + ") Q := v; end Set; "
           "  public method Get(): " + t + " => Q; "
           "  public method Twice(): " + t + " => Q + Q; "
           "  public method Bump() " + stmt + " end Bump; "
           "end E; "
           "class Program5061 public static method Main(): int "
           "  var e := new E(); e.Set(" + lit + "); e.Bump(); "
           "  Console.WriteLine(e.Get().ToString()); "
           "  Console.WriteLine(e.Twice().ToString()); "
           "  return 0; end Main; end Program5061;";
}
}  // namespace

BEZEL_TEST(bare_ident_inherited_generic_property_5061, int_read_write_compound) {
    auto r = runAot(prog("int", "40", "Q += 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "41\n82\n");
}

BEZEL_TEST(bare_ident_inherited_generic_property_5061, byte_read_write_compound) {
    auto r = runAot(prog("byte", "20", "Q += 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "21\n42\n");
}

BEZEL_TEST(bare_ident_inherited_generic_property_5061, long_read_write_compound) {
    auto r = runAot(prog("long", "5000000000L", "Q += 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5000000001\n10000000002\n");
}

BEZEL_TEST(bare_ident_inherited_generic_property_5061, double_read_write_compound) {
    auto r = runAot(prog("double", "1.5", "Q += 1.0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2.5\n5\n");
}

BEZEL_TEST(bare_ident_inherited_generic_property_5061, generic_subclass_of_generic_base) {
    auto r = runAot(
        "module M5061g; class B<T> public property Q: T; end B; "
        "class G<U>: B<U> "
        "  public method Set(v: U) Q := v; end Set; "
        "  public method Get(): U => Q; end G; "
        "class P public static method Main(): int "
        "  var a := new G<long>(); a.Set(5000000000L); "
        "  Console.WriteLine(a.Get().ToString()); "
        "  var b := new G<string>(); b.Set(\"hi\"); "
        "  Console.WriteLine(b.Get()); "
        "  return 0; end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5000000000\nhi\n");
}
