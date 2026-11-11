//! case: s28_2_4606
//! doc: spec
//! section: 28.2
//! title: a kill only in a catch arm does not make the use after the try an error
//! rule: A holder whose source was disposed on only some paths to the use
//! cites: 28.3
//! expect: run
//! stdout: ann
module Conf;

public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;

public record Pair(Item: Customer?);

public static class Sink
  public static method TakePair(p: Pair): int => 1;
end Sink;

public class Program
  public static method Main()
    var c := new Customer("ann");
    var b := c;
    var k := 1;
    try
      if k > 100 then
        throw new InvalidOperationException("boom");
      end;
    catch
      InvalidOperationException => c.Dispose(),
    end try;
    Console.WriteLine(b.Name);
  end Main;
end Program;
