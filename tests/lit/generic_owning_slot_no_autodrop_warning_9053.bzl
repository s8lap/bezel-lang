// fix #9053 (§28.3 K3.1, §28.11, §14.2, §14.4, v0.359): the §28.3 must-use
// warning must NOT fire on an `owned` parameter stored into an OWNING-
// COLLECTION slot whose element type is a type parameter.
//
// THE ISSUE. `_items[key] := item;` with `owned item: T` into an
// `OwningDictionary<K, T>` field inside `Repo<K, T> where T: class` drew
//
//   warning: owned parameter `item` is implicitly disposed at the end of the
//   method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, …
//
// while the non-generic twin is silent.
//
// THE SPEC. §28.3's consume table, row K3.1 (v0.359): "store into an `owned`
// field or owning-collection slot, including through a `set(owned)` accessor"
// is a MARKER-FREE consuming position whose post-state is "**moved-from** — a
// later read is an error until reassigned". So the store CONSUMES `item` and
// the compiler must not ALSO report it as implicitly disposed.
//
// ⚠ WHY THIS IS A lit ROW AND NOT A gtest. `runAot` fills
// `JitResult::diagnostics` only on a REJECT (issue #3138): the child's stderr
// is discarded on the success path. Every program in this file is LEGAL and
// compiles, so a `runAot`-based "the warning is ABSENT" assertion cannot see
// it — the oracle would be vacuously green on the base and green with the fix,
// proving nothing. `bezelc -c` writes every diagnostic of any severity to
// stderr on an accept, which is what the FileCheck run below reads. The
// warning-absence half of #9053 is pinned HERE and nowhere else; the
// gtest file tests/ownership/generic_owning_slot_consumes_owned_param_9053_test.cpp
// pins the two REJECTIONS (K3.1's moved-from post-state and §28.11's
// owned-slot rule), which `runAot` CAN see.
//
// THE ORACLE CANNOT PASS VACUOUSLY, and the argument is mechanical:
//
//   * The MUST-NOT-WARN side is the issue's own store (`_items[key] := item;`
//     inside `Repo<K, T>`). On the pre-fix base this program compiles and
//     prints the warning, so the CHECK-NOT below FAILS — proven mechanically
//     in the handback (the pre-fix compiler's own output against this file).
//   * The MUST-WARN side is the SIBLING that still owes the warning: the
//     same `owned item: T` stored into a NON-owning `Dictionary<K, T>`. A
//     borrowing collection copies the value into a slot it does not own
//     (§28.11), so the store does NOT consume `item` and the §28.3 obligation
//     stands — the warning is DUE and MUST still be emitted. That CHECK is
//     what CLOSES the CHECK-NOT's span, and it is also the proof that the
//     compiler still warns in this very file: a compiler that warned nowhere
//     would fail the MUST-WARN row, and one that warned on the generic owning
//     store would fail the MUST-NOT row. Only a compiler that warns on the
//     right LINE passes.
//
// The first RUN line pins exit 0 — this program is LEGAL, so the diagnostics
// below are WARNINGS ON A SUCCESSFUL COMPILE, not a rejection's error list.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

// §28.3 K3.1 (v0.359) — the ISSUE'S REPRO. The store into the
// `OwningDictionary<K, T>` slot ANNOUNCES the consume (marker-free), so
// `item` is moved-from and the §28.3 must-use lint must NOT fire.
//
// The element type is the type parameter `T`: §14.1 / #6530 record a
// declaration-less single-part Named TypeRef as the shape a type parameter
// shares with a built-in, and the stdlib's own `where T: class` / `where V:
// class` (stdlib §4.5 / §4.9) plus §14.4's instantiation-site restriction
// make every ADMISSIBLE instantiation's element class/interface — the
// element CATEGORY §28.11 / §3.6 rule 2 asks about.
public class Repo<K, T> where K: hashable where T: class
  private owned _items: OwningDictionary<K, T> := new OwningDictionary<K, T>();
  public method Add(key: K, owned item: T)
    _items[key] := item;
  end Add;
  public property Count: int => _items.Count;
end Repo;

// The SIBLING THAT MUST STILL WARN. A NON-owning `Dictionary<K, T>` copies
// the value into a slot it does not own (§28.11), so the store does not
// consume `item`: the §28.3 obligation stands, the auto-drop warning is DUE,
// and it must still be emitted after #9053's fix. This is the positive CHECK
// that closes the CHECK-NOT's span — without it the negative row could pass
// vacuously on a compiler that warns nowhere.
public class Borrow<K, T> where K: hashable where T: class
  public method Keep(owned item: T)
    var d: Dictionary<K, T> := new Dictionary<K, T>();
    d[1] := item;
  end Keep;
end Borrow;

public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    r.Add(1, new User(1));
    Console.WriteLine(r.Count);
    using var b := new Borrow<int, User>();
    b.Keep(new User(2));
    Console.WriteLine("done");
  end Main;
end Program;

// ===== MUST-NOT-WARN: the issue's own generic owning-collection store =====
// The `owned item: T` parameter of `Repo<K, T>.Add` — declared at
// `generic_owning_slot_no_autodrop_warning_9053.bzl:75:29` (the diagnostic
// points at the `owned` modifier, which is where the obligation originates).
// K3.1's post-state is "moved-from", so the §28.3 must-use lint has nothing
// left to say about that parameter.
//
// The needle is the warning's own TEXT, not a location, so the row fails if
// the warning is emitted AT ALL for this parameter — a warning that drifted
// onto another line, or onto the sibling's parameter, would still be caught.
//
// CHECK-NOT: owned parameter `item` is implicitly disposed at the end of the method

// ===== MUST-WARN: the non-owning `Dictionary<K, T>` sibling =====
// The `owned item: T` parameter of `Borrow<K, T>.Keep` — declared at
// `generic_owning_slot_no_autodrop_warning_9053.bzl:88:22`. A borrowing
// collection does not own its elements (§28.11), so the store consumes
// nothing and the owned parameter is undischarged: the §28.3 must-use warning
// is DUE.
//
// The location is pinned too, so the warning cannot silently move onto
// another statement of the same method.
//
// CHECK: generic_owning_slot_no_autodrop_warning_9053.bzl:88:22: warning: owned parameter `item` is implicitly disposed at the end of the method
// CHECK-SAME: (§28.3 must-use / auto-drop)
