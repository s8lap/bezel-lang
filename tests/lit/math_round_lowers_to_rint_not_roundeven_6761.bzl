// fix #6761 (stdlib §2.2 Math.Round, v0.340): Math.Round must NOT lower to
// llvm.roundeven. That intrinsic becomes an undefined libm `roundeven`, which
// musl 1.2.5 (the static release bundle) lacks, so every Math.Round program
// failed to link there. It lowers to llvm.rint (round-half-to-even in the
// default FP environment; `rint` exists in glibc and musl).
// Glibc-hosted behaviour tests cannot see this (identical output), so the
// pin is on the emitted IR and on the object's undefined symbols.
//
// RUN: env BEZEL_DUMP_IR=1 %bezelc -c -o %t.o %s 2> %t.ll
// RUN: FileCheck %s --check-prefix=IR < %t.ll
// RUN: FileCheck %s --check-prefix=NOIR < %t.ll
// RUN: llvm-nm %t.o | FileCheck %s --check-prefix=SYM
// RUN: llvm-nm %t.o | FileCheck %s --check-prefix=NOSYM

// both overloads reach the intrinsic (keeps the NOT passes from being vacuous)
// IR: call double @llvm.rint.f64
// IR: call double @llvm.rint.f64
// NOIR-NOT: roundeven
// SYM: U rint
// NOSYM-NOT: roundeven

module M;
class C
  public static method Main(): int
    Console.WriteLine(Math.Round(2.5));
    Console.WriteLine(Math.Round(2.675, 2));
    return 0;
  end Main;
end C;
