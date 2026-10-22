//! case: s35_3_0022
//! doc: spec
//! section: 35.3
//! title: a method of the choice itself uses the bare case in a declared-result expression body
//! rule: "where the declared type — nullability stripped — is a choice instantiation `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form `N(args)` is a case construction of `C<…>.N(args)`, and the bare identifier `N` is the payload-free case value `C<…>.N`" (§35.3, v0.340). Inside `Fetch`, `Reset(): Fetch => NotModified` is a declared-result expression body, so the bare name is the case value.
//! cites: 22.2
//! expect: run
//! output: nm
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
  public method Flip(): Fetch => (case this of Got(var b) => Fetch.NotModified, NotModified => Fetch.Got("f"));
  public method Reset(): Fetch => NotModified;
end Fetch;

public class Program
  public static method Main()
    var f := Fetch.Got("a");
    Console.WriteLine((case f.Reset() of Got(var b) => b, NotModified => "nm"));
  end Main;
end Program;
