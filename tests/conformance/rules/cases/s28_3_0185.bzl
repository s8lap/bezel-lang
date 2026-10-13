//! case: s28_3_0185
//! doc: spec
//! section: 28.3
//! title: a generic body may not return a lambda it formed with T in a parameter slot
//! rule: In a generic body, a function value **formed or retyped at a function
//!   type that places one of the body's type parameters, bare or nullable, in
//!   a parameter slot, at any depth** ... is bound to a local, invoked,
//!   captured by a closure, or passed to a function-typed parameter, and
//!   reaches no other position: not a `return` or `yield return`, a lambda's
//!   included (`Put(): (T) -> ()` returning `lambda(t) => Registry.Take(t)`
//!   ...), not a function-typed holder other than a `scoped` field ... not a
//!   set-erasing position (a conversion to `Object` included, and an argument
//!   or store at a position another generic declares at a bare type parameter
//!   which the body instantiates with such a function type — `new Keep<(T) ->
//!   ()>(lambda(t: T) => …)`)
//! cites: 28.3, 27.3
//! expect: reject
//! diagnostic: type parameter
//! issue: 5829
module Conf;

public class Box<T>
  public constructor()
  end constructor;

  public method Put(): (T) -> ()
    return lambda(t) => Console.WriteLine("put");
  end Put;
end Box;
