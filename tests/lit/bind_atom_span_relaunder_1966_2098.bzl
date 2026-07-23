// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (#1966) × §17.15 (#2098, review F3): the binding atom
// must not RE-LAUNDER the indirect span borrow-escape #1966 closed. The
// atom's value is its body's, so both re-spellings of the one-hop holder
// return are the same dangling view:
//   (A) `return (var n := 1; s);`            — a statement-level span holder
//       returned through the atom (checkIndirectEscapeOnReturn peels the
//       atom to its body via common::peelBindAtomToBody);
//   (B) `return (var s := local.AsSpan(); s);` — the holder IS an atom
//       clause (the pre-scan's BindExpr arm collects the clause's carried
//       provenance via collectRecordBorrowSource, exactly as the statement
//       LocalDecl arm does).
module M;
class C
  public static method A(): ArraySpan<int>
    var local := new Array<int>();
    local.Add(7);
    var s := local.AsSpan();
    return (var n := 1; s);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: returned `s` is a span viewing `local` whose backing storage is disposed at scope end
    // CHECK-SAME: (§9.7 borrow-lifetime rule
  end A;

  public static method B(): ArraySpan<int>
    var local := new Array<int>();
    local.Add(7);
    return (var s := local.AsSpan(); s);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: returned `s` is a span viewing `local` whose backing storage is disposed at scope end
  end B;
end C;
