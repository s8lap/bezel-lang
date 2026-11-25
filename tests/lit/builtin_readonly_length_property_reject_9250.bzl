// fix #9250 (stdlib §9.6 / §3.1 / §4.14, §23.3, §17.6, v0.359): an
// assignment to a `string`'s / a `StringBuilder`'s / an `OwningBuffer<T>`'s
// read-only `Length` compiled with only an `internal:` warning (#4045) and the
// store was silently dropped.
//
// stdlib §9.6 declares `public pure readonly property Length: int;` on
// `String`; stdlib §3.1 declares `public readonly property Length: int;` on
// `StringBuilder`; stdlib §4.14 declares `public pure readonly property
// Length: int;` on `OwningBuffer<T>` — three GET-ONLY properties. Language
// spec §23.3 makes a property with no setter read-only ("A read-only property
// in block form omits `set`"), §23.1 admits a `readonly` auto-property's
// assignment only in a field initializer or a constructor of the declaring
// type (which none of these compiler-intrinsic types has), and §17.6 says
// "Read-only locations cannot be incremented", so every spelling below is a
// compile error.
//
// The diagnostic is the compiler's ORDINARY read-only-property one — the
// same text a user get-only property gets.
//
// The `?.` receiver twin (`t?.Length := 3;`) is NOT here: A.6's lvalue shape
// already rejects that spelling with its own §7.5 diagnostic, so the fix does
// not reach it.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module P; import System;
public class Program
  public static method Main(): int
    var t := "abc";
    t.Length := 3;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    t.Length += 1;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    t.Length++;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot increment/decrement read-only property `Length` — it has no setter (§17.6 / §23.3)
    var sb := new StringBuilder(); sb.Append("hey");
    sb.Length := 3;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    sb.Length++;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot increment/decrement read-only property `Length` — it has no setter (§17.6 / §23.3)
    using var b := new OwningBuffer<byte>(4);
    b.Length := 3;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot assign to read-only property `Length` — it has no setter (§23.3)
    b.Length++;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot increment/decrement read-only property `Length` — it has no setter (§17.6 / §23.3)
    return 0;
  end Main;
end Program;
