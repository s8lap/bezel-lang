// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #5328 / spec §15.4 (v0.313) — CONTROL ROW: the SAME-MODULE twin of
// `imported_literal_target_5328__inline_array_imported.bzl`.
//
// §15.4 admits an inline-array target, and this is the leg that has always
// compiled clean (the issue measures it: "same module compiles and prints the
// right sum"). It is pinned so the imported row's `NODIAG` assertion is
// anchored to a known-good local baseline: if a future change made the local
// leg reject, this row would say so, and the imported row's silent clean
// compile would be an admission-by-omission rather than a claim. The two
// legs differing is precisely the defect #5328 item 2 describes.
module App5328InlLocal;

public class Ops
  public static method Inl(a: [3]int): int => a[0] + a[1] + a[2];
end Ops;

public class Program
  public static method Main(): int
    Console.WriteLine(Ops.Inl([1, 2, 3]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: internal:
