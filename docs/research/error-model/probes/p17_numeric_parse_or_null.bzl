// Probe p17 (informational; rejected on r40). The *OrNull tier is a function of
// the throwing tier (stdlib §9.1: "null exactly where Parse(s) raises"), yet it
// still has to be implemented once per type. r40 has it for bool and Guid and
// not for the integer and floating types: the three numeric lines are errors,
// the other two are not.
module P17;

public class Program
  public static method Main()
    Console.WriteLine(bool.ParseOrNull("yes") = null);
    Console.WriteLine(Guid.ParseOrNull("zz") = null);
    Console.WriteLine(int.ParseOrNull("12x") = null);
    Console.WriteLine(long.ParseOrNull("12x") = null);
    Console.WriteLine(double.ParseOrNull("1.5x") = null);
  end Main;
end Program;
