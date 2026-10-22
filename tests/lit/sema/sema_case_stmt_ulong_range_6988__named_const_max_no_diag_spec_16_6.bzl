// #6988 (§19.3, §5.1, v0.340): a statement-form range whose upper bound is a named
// ulong constant holding the ulong maximum must compile with no diagnostic. The
// constant is read by the gate through its recorded type, not its folded value.
//
// Non-vacuity: the first RUN line requires the program to compile (exit 0).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public const K: ulong := 18446744073709551615UL;
  public static method Main()
    var u: ulong := 3;
    case u of
      0..K => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
