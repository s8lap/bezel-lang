# n12-repo

*Area:* generics

A generic repository: `interface IEntity<K> readonly property Id: K;` and `class Repo<K, T> where K: hashable where T: class, IEntity<K>` backed by an `OwningDictionary<K, T>`, with `Add(owned item)`, `FindOrNull(id)`, `Where(pred)` returning the matching ids as an array, `Remove(id): bool`. Store three `User(id, name, active)` objects; print `find 2 -> bob`, `find 9 -> none`, `active: 1, 3`, `removed 1: True`, `count 2`.
