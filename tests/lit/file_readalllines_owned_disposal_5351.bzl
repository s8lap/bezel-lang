// fix #5351 (§28.9, v0.313): File.ReadAllLines returns `owned Array<string>`,
// but producerState was returning Borrowed for this builtin static factory call,
// so the local `var lines` carried no disposal obligation and the Array and
// its string elements leaked silently (1386 B / 99 allocs per 100 iterations).
// The fix extends producerState to return Owned for all builtin static factory
// calls registered in kOwnedStaticFactories (File.ReadAllLines, File.ReadAllBytes,
// Directory.GetFiles, Directory.GetDirectories, FrozenArray<T>.From,
// Stopwatch.StartNew, CancellationTokenSource factories).
//
// This file verifies that a File.ReadAllLines result stored in a plain `var`
// local (implicit scope-end disposal) compiles, and that the using var form
// does too.
//
// §28.9 (v0.357, fix #4418): `Array<string>` is trivial-dispose, so the
// ceremony exemption silences the §28.3 must-use warning at the implicit-drop
// site of a plain-`var` local bound to this `owned`-returning call (the
// produced type keys the exemption). This row pinned that warning as its
// proof the obligation was registered; the spec flip reverses it, and the
// scope-end free itself is covered by the leak tests for #5351.
//
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -c -o %t.o %s

module M5351;

class C5351
  public static method TestPlainVar(): int
    // `lines` is Owned and auto-dropped at scope end; the trivial-dispose
    // ceremony exemption (§28.9) draws no warning.
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

// NODIAG-NOT: warning:
