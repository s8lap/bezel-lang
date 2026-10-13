//! case: s28_3_0187
//! doc: spec
//! section: 28.3
//! title: a record component declared at a function type over T is admitted
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
//!   ... Declarations are not restricted: a return type, field, property or
//!   record component may be declared at `(T) -> …`
//! cites: 28.3, 27.3
//! expect: accept
module Conf;

public record H<T>(F: (T) -> bool);

public class User
  public static method Run(): bool
    var h := new H<int>(lambda(x) => x > 0);
    var g := h.F;
    return g(3);
  end Run;
end User;
