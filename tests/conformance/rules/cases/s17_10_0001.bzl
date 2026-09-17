//! case: s17_10_0001
//! doc: spec
//! section: 17.10
//! title: typeof of a user type returns its Type value with Name and FullName
//! rule: "`typeof(TypeName)` returns the `Type` value for a statically named
//!   type: `var t: Type := typeof(BankAccount); Console.WriteLine(t.Name); //
//!   \"BankAccount\"`."
//! cites: 17.10
//! expect: run
//! stdout: BankAccount
module Conf;

public class BankAccount
  public constructor()
  end constructor;
end BankAccount;

public class Program
  public static method Main()
    var t: Type := typeof(BankAccount);
    Console.WriteLine(t.Name);
  end Main;
end Program;
