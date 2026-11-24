# n11-comparers

*Area:* generics + borrows

Since closures cannot be returned, compose comparers as objects: an interface `IOrder<T>` with `Compare(a, b): int`, classes `ByKey`-style concrete orders for `Person(Name, Age, City)` (`ByCity`, `ByAgeDesc`, `ByName`), and a `ThenOrder<T>` that holds borrows of two other orders. Sort 5 people with `ThenOrder(ByCity, ThenOrder(ByAgeDesc, ByName))` through `Array.Sort(lambda(a, b) => order.Compare(a, b))`. Print the sorted list `Bergen 25 bob`, `Bergen 25 eve`, `Oslo 40 cho`, `Oslo 30 ann`, `Oslo 30 dee`. Record the trust-report lines.
