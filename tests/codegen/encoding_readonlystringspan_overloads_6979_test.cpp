// fix #6979 -- codegen: Encoding.GetByteCount/GetBytes over a
// ReadOnlyStringSpan source (stdlib §3.2, v0.340). The three span overloads
// failed the IR verifier: the {ptr,i32,i32} %bz.arrayspan value was passed to
// bz_encoding_utf8_get_bytes / _get_byte_count / _write_bytes, which take a
// string. The span is now materialized to a code-point-window string (and
// released) before the runtime call. runAotChecked = generated-code ASan +
// LeakSanitizer; the loop runs 100x so a leaked window is caught.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// Window over "aé€z" = {U+00E9 (2 bytes), U+20AC (3 bytes)}.
const char* kPrelude =
    "module M; class C public static method Main(): int "
    "  var total := 0; var last := 0; "
    "  for var i in 0..99 do "
    "    var rs := new ReadOnlyStringSpan(\"a\\u00E9\\u20ACz\", 1, 2); ";
}  // namespace

BEZEL_TEST(encoding_readonlystringspan_overloads_6979, getbytecount_span_3_2) {
    auto r = runAotChecked(std::string(kPrelude) +
        "    total := total + Encoding.UTF8.GetByteCount(rs); "
        "  end; "
        "  Console.WriteLine(total); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("500\n"));
}
BEZEL_TEST(encoding_readonlystringspan_overloads_6979, getbytes_span_allocating_3_2) {
    auto r = runAotChecked(std::string(kPrelude) +
        "    using var b := Encoding.UTF8.GetBytes(rs); "
        "    total := total + b.Count; last := b[1] * 1000 + b[2]; "
        "  end; "
        "  Console.WriteLine(total); Console.WriteLine(last); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // U+00E9 = C3 A9 (195, 169); U+20AC = E2 82 AC.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("500\n169226\n"));
}
BEZEL_TEST(encoding_readonlystringspan_overloads_6979, getbytes_span_into_destination_3_2) {
    auto r = runAotChecked(std::string(kPrelude) +
        "    using var dst := new Array<byte>(8, 0); "
        "    var n := Encoding.UTF8.GetBytes(rs, dst.AsSpan()); "
        "    total := total + n; last := dst[0] * 1000000 + dst[1] * 1000 + dst[2]; "
        "  end; "
        "  Console.WriteLine(total); Console.WriteLine(last); return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("500\n195169226\n"));
}
// Control: the string overloads still behave identically.
BEZEL_TEST(encoding_readonlystringspan_overloads_6979, string_overloads_unchanged_3_2) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var s := \"a\\u00E9\\u20ACz\"; "
        "  Console.WriteLine(Encoding.UTF8.GetByteCount(s)); "
        "  using var b := Encoding.UTF8.GetBytes(s); Console.WriteLine(b.Count); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n7\n"));
}
