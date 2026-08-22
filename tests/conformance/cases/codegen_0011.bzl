lambda(x) => x * 2                          // parameter types inferred from the target type
lambda() => 0                               // empty parens MANDATORY
lambda(x: int): long => x * 2               // return-type ascription
lambda(item: Item) => do var n := N(item); return n; end
async lambda(u: string) => await Fetch(u)
lambda(_, x) => x                           // `_` ignore; duplicates legal
lambda(Entry(k, v)) => $"{k}: {v}"          // positional-record destructuring parameter (§27.4)
lambda(owned h) => _handlers.Add(h)         // modifiers when the target type declares them

(int, int) -> int      () -> ()      (owned T) -> ()      (ref int) -> ()
(out owned T) -> ()    ((int) -> bool)?                   // parens required before `?`
