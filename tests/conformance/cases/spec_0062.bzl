var xs: Array<int> := [1, 2, 3];

var doubled := xs.Select(lambda(x) => x * 2);      // block T = int from the receiver (§26.5);
                                             //   phase 2: lambda param x : int, body int ⇒ U = int
var byKey := xs.OrderBy(lambda(x) => x mod 3);     // TKey = int (phase 2); the §14.2 constraint
                                             //   check (IComparable<int>) runs after inference
var total := xs.Aggregate(0L, lambda(acc, x) => acc + x);
                                             // TAccum = long from the seed (phase 1);
                                             //   lambda checks as (long, int) -> long (phase 2)
var empty: Array<string> := Util.Empty();    // T = string from the target (phase 3)
var n := Util.CountOf(xs);                   // CountOf<T>(xs: IEnumerable<T>): int —
                                             //   Array<int> vs IEnumerable<T> ⇒ T = int (phase 1)
// var bad := Util.Pick(1, 2L);              // error: T bound to both int and long — agree, or
                                             //        Pick<long>(1, 2L)
// var loose := Util.Empty();                // error: T appears only in the return type —
                                             //        target or explicit argument required
