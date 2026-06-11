using var thing: Animal := Acquire();          // owned
var maybeDog := thing as Dog;                  // borrow; thing still owned
if maybeDog <> null then
  // maybeDog is a borrowed Dog reference; thing remains the owner.
  // To transfer ownership, the standard owned-local rules of §28.3 apply
  // to `thing`, not to the result of `as`.
end;
