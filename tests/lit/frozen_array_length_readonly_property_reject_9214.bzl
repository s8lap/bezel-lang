// fix #9214 (stdlib §4.19, §23.3, §17.6, v0.359): an assignment to a
// `FrozenArray<T>`'s read-only `Length` compiled with only an `internal:`
// warning (#4045) and the store was silently dropped.
//
// stdlib §4.19 declares `public readonly property Length: int;` on
// `FrozenArray<T>` — a GET-ONLY property. Language spec §23.3 makes a
// property with no setter read-only ("A read-only property in block form
// omits `set`"), and §17.6 says "Read-only locations cannot be incremented",
// so both spellings below are compile errors.
//
// The diagnostic is the compiler's ORDINARY read-only-property one — the
// same text a user get-only property gets.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module P; import System;
public class Program
  public static method Main(): int
    var s := new Array<int>(); s.Add(7); s.Add(2);
    using var f := FrozenArray<int>.From(s);
    f.Length := 3;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    f.Length += 1;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    f.Length++;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot increment/decrement read-only property `Length` — it has no setter (§17.6 / §23.3)
    return 0;
  end Main;
end Program;
