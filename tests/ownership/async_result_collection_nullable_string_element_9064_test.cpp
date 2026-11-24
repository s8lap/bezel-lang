// Codegen / disposal — the ASYNC Pattern-A result-collection disposer released
// a +1 per `string?` element for EVERY collection kind, while the LINKED-LIST
// arm's per-element RETAIN is still `!nullable`-gated. So an async method
// returning `Task<LinkedList<string?>>` that holds a borrowed element released
// a reference nothing had retained: "bezel: fatal: refcount over-release
// (string) — two releases for one reference" (rc 134, the double-free
// direction). The release is now gated BY COLLECTION KIND: it is installed for
// the kinds whose matching retain is in place — the array-backed family
// (Array / Stack / Queue) and the borrowing Dictionary's VALUE — and the
// LINKED-LIST arm keeps its `!nullable` gate.
//
// Spec: §3.6 "Derived type properties" *Nullable forms* (the ELEMENT CATEGORY
// of a nullable type `V?` is `V`'s, nullability stripped), §28.9 "Runtime-
// managed types" (a `string` is refcounted; null is a retain/release no-op),
// §28.11 "Collection element ownership" *The element slot policy* (the
// runtime-managed-reference row's borrowing-family column is "shared
// references": the collection owns a +1 of its own on the element and releases
// it when the slot goes away — and the slot policy is a PAIR: the store takes
// the reference, the collection's free gives it back). Spec revision v0.359.
//
// Fix-forward of the rejected commit feb164d20 (branch r43-m41-9064), whose
// Disposal.cpp hunk removed the `!nullable` gate for every kind.
//
// Every string is built at run time (a folded literal is an RC sentinel on
// which release is a no-op and would hide the leak). runAotChecked runs under
// ASan/LSan, so both a leak (the bug) and a double-free / use-after-free flip
// `r.ran`; stdout pins the values.
//
// ROW BASES (each row is RED without the fix that owns it):
//   * linkedlist_borrowed_element_... — RED on feb164d20 (the over-release
//     abort); green on the base r30-c1 and on this fix-forward.
//   * dictionary_borrowed_value_... — RED on the base r30-c1 (the borrowing
//     Dictionary's `string?` value leaked: the `.Add` retain was in place but
//     the free gave nothing back); green with feb164d20 + this fix-forward.
//   * the two control rows are green on every arm; each is RED under the
//     wrong fix the commit body names.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// An async method that stores its BORROWED `string` parameter in a
// `LinkedList<string?>` and returns it as the Pattern-A task result. The
// linked list's per-element retain is `!nullable`-gated, so the element is a
// borrow; the task result's free must NOT release it.
const char* kAsyncLinkedListBorrowed =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(s: string): Task<LinkedList<string?>>\n"
    "    var r := new LinkedList<string?>();\n"
    "    r.Add(s);\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      using var t := C.Make(s);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count + s.Length;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The borrowing Dictionary's VALUE is the other kind whose retain is in place
// (`dictIsStringValue`, #8207 — no `!nullable` gate), so its task result's
// free DOES give the +1 back.
const char* kAsyncDictionaryBorrowed =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(s: string): Task<Dictionary<int, string?>>\n"
    "    var r := new Dictionary<int, string?>();\n"
    "    r.Add(1, s);\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      using var t := C.Make(s);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count + s.Length;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the NON-NULLABLE linked-list twin. Its element IS retained (the
// `!nullable` gate admits it), so the task result's free gives that +1 back —
// this row is RED if the whole linked-list release is dropped instead of
// gated on the element's nullability.
const char* kControlLinkedListNonNullable =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(s: string): Task<LinkedList<string>>\n"
    "    var r := new LinkedList<string>();\n"
    "    r.Add(s);\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      using var t := C.Make(s);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count + s.Length;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the NON-NULLABLE dictionary twin. Its value IS retained and
// released — this row is RED if the borrowing dictionary's value release is
// dropped along with the nullable one.
const char* kControlDictionaryNonNullable =
    "module M; import System;\n"
    "public class C\n"
    "  public static async method Make(s: string): Task<Dictionary<int, string>>\n"
    "    var r := new Dictionary<int, string>();\n"
    "    r.Add(1, s);\n"
    "    return r;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var s := \"w\" + i.ToString();\n"
    "      using var t := C.Make(s);\n"
    "      t.Wait();\n"
    "      tot := tot + t.Result.Count + s.Length;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug) and no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(ownership_async_result_collection_nullable_string_element_9064,
           linkedlist_borrowed_element_is_not_over_released_spec_28_11) {
    expectClean(kAsyncLinkedListBorrowed, "890\n");
}

BEZEL_TEST(ownership_async_result_collection_nullable_string_element_9064,
           dictionary_borrowed_value_is_released_spec_28_11) {
    expectClean(kAsyncDictionaryBorrowed, "890\n");
}

BEZEL_TEST(ownership_async_result_collection_nullable_string_element_9064,
           control_linkedlist_nonnullable_twin_unchanged_spec_28_9) {
    expectClean(kControlLinkedListNonNullable, "890\n");
}

BEZEL_TEST(ownership_async_result_collection_nullable_string_element_9064,
           control_dictionary_nonnullable_twin_unchanged_spec_28_9) {
    expectClean(kControlDictionaryNonNullable, "890\n");
}
