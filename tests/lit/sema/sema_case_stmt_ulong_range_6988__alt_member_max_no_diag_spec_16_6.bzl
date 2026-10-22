// #6988 (§19.3, §5.1, v0.340): a statement-form alternative list whose range
// member has a ulong upper bound at the maximum (`7UL | 2^63..2^64-1`) must
// compile with no diagnostic.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public static method Main()
    var u: ulong := 18446744073709551615UL;
    case u of
      7UL | 9223372036854775808UL..18446744073709551615UL => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
