// Spec-derived regression test for issue #5003 (spec §2.2 / §2.3, v0.307).
//
// THE GOVERNING RULE. Spec §2.2 "Aliases" / "Collisions" (v0.307): "An alias
// may not collide with another alias, an imported module's name segment in
// qualifier position, a type declared in the file's module, or an
// **unambiguously imported public bare name**: §2.7 rule 1 resolves imported
// bare names before aliases with no backtracking, so a same-named alias
// would be unreachable in every chain-head position — the collision is an
// error at the alias declaration rather than a silently dead alias."
//
// ROOT CAUSE. The alias / imported-bare-name check in `Resolver::run`
// (`src/sema/TypeResolution.cpp`'s §2.2 v0.183 #1515 block) only knew names
// from `ctx_.importedTypesOf(dotted)` — loaded dependency artifacts only.
// Stdlib pseudo-modules (`System.Threading`, `System.IO`, …) and every §2.3
// always-available bare name (`Task`, `Console`, `Mutex` once
// `System.Threading` is imported, …) are resolved elsewhere, through the
// builtin-name tables (`builtinTypeArity`, `builtinClassModulePathOf`, the
// expression-position receiver list in `ExprTypecheck_Exprs.cpp`). So the
// alias collision was never reported at the alias declaration site — the
// alias bound the local name to the imported module, and §2.7 rule 1's
// no-backtracking lookup silently resolved the bare name to the always-
// available / imported-public reading in every chain-head position, leaving
// the alias itself unreachable.
//
// THE FIX. Extend `Resolver::run`'s §2.2 alias-collision block with two
// additional checks after the existing imported-bare-name check (#4690's
// gate-widening):
//   1. Is the alias an §2.3 always-available bare name? → REJECT
//      (visible in every file; §2.7 rule 1 wins regardless of imports).
//   2. Is the alias a public of the EXPLICITLY IMPORTED stdlib pseudo-
//      module (per `builtinClassModulePathOf`)? → REJECT (e.g.
//      `import System.Threading as Mutex;`).
// Both cases cite §2.2 / §2.7 rule 1.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff compilation failed with an ERROR diagnostic carrying `needle`.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the program compiles, links, runs and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

}  // namespace

// §2.2 / §2.3 (v0.307) #5003 — T1: `import System.Threading as Task;` —
// Task is always-available per §2.3 row 5 (the Task family is visible
// without import). The alias would be unreachable in every chain-head
// position; the declaration must be rejected.
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_task_rejected_5003) {
    const char* src =
        "module Conf; "
        "import System.Threading as Task; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "Task"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// §2.2 / §2.3 (v0.307) #5003 — T2: `import System as Foo; import Foo as
// Console;` — Console is always-available per §2.3 row 8 (System is
// auto-imported; `Console` is a member). Same shape as T1: the alias
// would be unreachable in every chain-head position. This case exercises
// the §2.3-row-8 path — Console is a static-class receiver, not a
// TypeRef-resolvable type, so the fix's always-available check has its
// own explicit list including Console.
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_console_rejected_5003) {
    const char* src =
        "module Conf; "
        "import System as Foo; "
        "import Foo as Console; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "Console"));
}

// §2.2 / §2.3 (v0.307) #5003 — T3: control — `import System as Foo;`
// `import Foo as Bar;` — aliasing through a chain (System → Foo → Bar)
// does NOT collide with anything. Must compile cleanly (exits 0 with `7`).
BEZEL_TEST(sema_import_alias_collision_5003,
           nonclashing_alias_accepted_5003) {
    const char* src =
        "module Conf; "
        "import System as Foo; "
        "import Foo as Bar; "
        "public class Program "
        "  public static method Main(): int return 7; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(runs(7, src));
}

// §2.2 / §2.3 (v0.307) #5003 — T6: `import System as Foo; import Foo as
// Subscription;` — Subscription is always-available per §2.3 row 6 (the
// events/subscription/cancellation set is visible without import).
// Exercises a DIFFERENT §2.3-row-6 name than T1's Task (which is row 5).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_subscription_rejected_5003) {
    const char* src =
        "module Conf; "
        "import System as Foo; "
        "import Foo as Subscription; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
}

// §2.2 / §2.3 (v0.307) #5003 — T7: `import System.Threading as Mutex;` —
// Mutex is NOT always-available (per §2.3 L160, "it is required for
// CancellationTokenSource, Mutex, and the other threading types"), but it
// IS a public of the EXPLICITLY IMPORTED stdlib pseudo-module
// System.Threading (per `kBuiltinClassModulePaths`). So this exercises the
// SECOND branch of the fix (the `builtinClassModulePathOf(alias) ==
// dotted` check), not the first (the always-available check). Mutex is
// NOT in the always-available name set (the §2.3 table only lists the
// `Task` family, `Subscription`, `CancellationToken`, and the six event
// types from System.Threading as always-available; the rest of
// System.Threading's types require import).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_imported_stdlib_public_mutex_rejected_5003) {
    const char* src =
        "module Conf; "
        "import System.Threading as Mutex; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "public `Mutex`"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "System.Threading"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// §2.2 / §2.3 (v0.307) #5003 — T8: `import System as Foo; import Foo as
// File;` — File is NOT always-available per §2.3 (System.IO requires
// import), but the file imports System, so `File` resolves via the
// always-available System-resident set only if it's listed there. Per the
// fix's explicit enumeration, `File` is NOT in the always-available set
// (File lives in System.IO and requires explicit import). So neither the
// always-available branch nor the stdlib-pseudo-module branch fires — the
// alias is ACCEPTED. This is a control case showing the boundary of the
// always-available name set: an import-gated stdlib pseudo-module type
// remains a valid alias target when the alias doesn't conflict with any
// other bare-name lookup.
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_to_import_gated_stdlib_public_accepted_5003) {
    const char* src =
        "module Conf; "
        "import System as Foo; "
        "import Foo as File; "
        "public class Program "
        "  public static method Main(): int return 13; end Main; "
        "end Program;";
    // The user's `Foo` does NOT export `File` (System.IO is a separate
    // module). The alias binds `File` to the `Foo` alias chain. Since
    // the user did not import System.IO, `File` is NOT an unambiguously
    // imported bare name here, so the alias is acceptable.
    BEZEL_EXPECT_TRUE(runs(13, src));
}

// §2.2 / §2.3 (v0.307) #5020 — follow-up to #5003. The
// `isAlwaysAvailableBareName` table introduced by #5003 (spec §2.3 row 10,
// "the whole of stdlib §4") was INCOMPLETE: 9 names from the row's
// "stated in full" enumeration were omitted, so `import X as <Name>;`
// for those 9 was silently accepted. Per spec §2.2 (v0.307), each of
// them must be rejected with the alias-collision diagnostic, just like
// T1's `import System.Threading as Task;`. The 9 names below are
// exactly the set §2.3 row 10 lists that were not in the #5003 table:
//
//   BitReader, BitWriter, DictionaryEntry, DrainedEntry, Counter,
//   OwningSlotMap, SlotHandle, ArrayBitReader, ArrayBitWriter
//
// (KeyNotFoundException is also named in §2.3 row 10, but it is already
// enumerated in the §2.3 row 7 set — every §31.4 standard exception —
// so it was already covered by the #5003 fix; it is not retested here.)
//
// Each test below uses the T1 shape (`import System.Threading as <Name>;`)
// because the always-available check fires at the alias DECLARATION
// (line 2) regardless of what the imported module is — the check is on
// `imp->alias`, not on the imported module's contents. System.Threading
// is used uniformly for clarity. The diagnostic needle is
// "always-available bare name", the canonical §2.2 alias-collision
// diagnostic from the always-available branch of the #5003 fix.

// T9: `import System.Threading as BitReader;` — BitReader is always-
// available per §2.3 row 10 (stdlib §4.21, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_bit_reader_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as BitReader; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "BitReader"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T10: `import System.Threading as BitWriter;` — BitWriter is always-
// available per §2.3 row 10.
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_bit_writer_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as BitWriter; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "BitWriter"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T11: `import System.Threading as DictionaryEntry;` — DictionaryEntry is
// always-available per §2.3 row 10 (stdlib §4.18, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_dictionary_entry_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as DictionaryEntry; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "DictionaryEntry"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T12: `import System.Threading as DrainedEntry;` — DrainedEntry is
// always-available per §2.3 row 10 (stdlib §4.18, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_drained_entry_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as DrainedEntry; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "DrainedEntry"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T13: `import System.Threading as Counter;` — Counter is always-
// available per §2.3 row 10 (stdlib §4, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_counter_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as Counter; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "Counter"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T14: `import System.Threading as OwningSlotMap;` — OwningSlotMap is
// always-available per §2.3 row 10 (stdlib §4, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_owning_slot_map_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as OwningSlotMap; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "OwningSlotMap"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T15: `import System.Threading as SlotHandle;` — SlotHandle is always-
// available per §2.3 row 10 (stdlib §4, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_slot_handle_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as SlotHandle; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "SlotHandle"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T16: `import System.Threading as ArrayBitReader;` — ArrayBitReader is
// always-available per §2.3 row 10 (stdlib §4.21, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_array_bit_reader_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as ArrayBitReader; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "ArrayBitReader"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}

// T17: `import System.Threading as ArrayBitWriter;` — ArrayBitWriter is
// always-available per §2.3 row 10 (stdlib §4.21, "stated in full" list).
BEZEL_TEST(sema_import_alias_collision_5003,
           alias_collides_with_always_available_array_bit_writer_rejected_5020) {
    const char* src =
        "module Conf; "
        "import System.Threading as ArrayBitWriter; "
        "public class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, "always-available bare name"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "ArrayBitWriter"));
    BEZEL_EXPECT_TRUE(rejectsWith(src, "\xC2\xA7""2.2"));
}
