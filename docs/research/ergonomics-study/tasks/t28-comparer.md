# t28-comparer

*Area:* generics & interfaces

Sort people (name, age, city) by city ascending, then age descending, then name, using a reusable comparer composed from key functions. Print the sorted list `city age name`. Also a generic `MaxBy<T, K>(xs, key)` helper returning the element with the largest key: print `oldest: <name>`.
