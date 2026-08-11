// fix #3044 (§17.11 / §3.6 rule 2, v0.242) OVER-REJECTION GUARD — the sharpest
// one in this family. The rejection is keyed to the BODY actually taking
// `default(T)`, NOT to `where T: value` alone. A `where T: value` generic whose
// body never defaults `T` places no demand on `T`'s zero, so instantiating it
// with a zero-less enum stays legal — the value arrives through the constructor,
// exactly as `Box<ZeroLessEnum>` does.
//
// Reading §17.11's closing rule as "every `where T: value` instantiation with a
// zero-less enum is an error" would reject this program, which no spec clause
// forbids.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

static class U
  public static method Echo<T>(v: T): T where T: value return v; end Echo;
end U;

class C public static method Main(): int return int(U.Echo<Level>(Level.Mid)); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
