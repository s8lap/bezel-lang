// #6988 (§19.3, §5.1, v0.340): a relational statement-form pattern whose operand
// is the ulong maximum (`< 2^64-1`) must compile with no diagnostic.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public static method Main()
    var u: ulong := 3;
    case u of
      < 18446744073709551615UL => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
