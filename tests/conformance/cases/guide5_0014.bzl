using var filter := LoadFilter();                          // owned; the lambda borrows it
var q := readings.Where(lambda(r) => filter.Admits(r));    // q retains the lambda —
using var kept := q.ToArray();                             //   and its borrow of `filter` —
                                                           //   until this consumption
