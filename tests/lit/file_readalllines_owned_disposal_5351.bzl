// fix #5351 (§28.9, v0.313): File.ReadAllLines returns `owned Array<string>`,
// but producerState was returning Borrowed for this builtin static factory call,
// so the local `var lines` carried no disposal obligation and the Array and
// its string elements leaked silently (1386 B / 99 allocs per 100 iterations).
// The fix extends producerState to return Owned for all builtin static factory
// calls registered in kOwnedStaticFactories (File.ReadAllLines, File.ReadAllBytes,
// Directory.GetFiles, Directory.GetDirectories, FrozenArray<T>.From,
// Stopwatch.StartNew, CancellationTokenSource factories).
//
// This file verifies that the warning fires correctly when a File.ReadAllLines
// result is stored in a plain `var` local (implicit scope-end disposal), and
// that the using var form also triggers disposal.
//
// RUN: %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %bezelc -c -o %t.o %s

module M5351;

class C5351
  public static method TestPlainVar(): int
    // With the fix, `lines` is Owned and triggers §28.3 auto-drop warning.
    // The warning confirms the ownership obligation is registered.
    var lines := File.ReadAllLines("/tmp/test.txt");
    return lines.Count;
  end TestPlainVar;

  public static method TestUsingVar(): int
    // using var: explicit scope-bound disposal
    using var lines := File.ReadAllLines("/tmp/test.txt");
    return lines.Count;
  end TestUsingVar;

  public static method Main(): int
    return TestPlainVar() + TestUsingVar();
  end Main;
end C5351;

// CHECK: warning: owned value in `lines` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop)
