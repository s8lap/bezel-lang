// #6988 (§16.6, §5.1, v0.340): a `case` STATEMENT over a ulong scrutinee accepts
// a range whose upper bound is the ulong maximum. The bound folds to -1 as a long
// long; the statement-form gate must read it back as unsigned, as the CaseExpr
// form does, and must not reject it with the §5.1 error that names `-1`.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0), so the
// silent second RUN line is checked against an accepted program.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public static method Main()
    var u: ulong := 3;
    case u of
      0..18446744073709551615UL => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
