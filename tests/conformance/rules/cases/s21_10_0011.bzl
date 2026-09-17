//! case: s21_10_0011
//! doc: spec
//! section: 21.10
//! title: a local type's name leaves scope at the end of its enclosing block
//! rule: A local type's name is in scope from its declaration statement to the
//!   end of the innermost enclosing statement block — exactly a local
//!   variable's scope.
//! cites: 21.10, 21.6, C.1
//! expect: reject
//! diagnostic: scope
module Conf;

public class Program
  public static method Main()
    do
      record Row(Name: string);
      var inner := new Row("inner");
      Console.WriteLine(inner.Name);
    end;
    var outer := new Row("outer");
    Console.WriteLine(outer.Name);
  end Main;
end Program;
