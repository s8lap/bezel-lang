//! case: l2_1_0002
//! doc: stdlib
//! section: 2.1
//! title: WriteLine(Object) invokes the argument's ToString override
//! rule: The Object overloads of Console.Write/WriteLine invoke ToString().
//! expect: run
//! stdout: Ticket(A1)
module Conf;

public class Ticket
  private readonly _code: string;

  public constructor(code: string)
    _code := code;
  end constructor;

  public override method ToString(): string => $"Ticket({_code})";
end Ticket;

public class Program
  public static method Main()
    var t := new Ticket("A1");
    Console.WriteLine(t);
  end Main;
end Program;
