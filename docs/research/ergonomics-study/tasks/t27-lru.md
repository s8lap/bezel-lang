# t27-lru

*Area:* generics & interfaces

A generic LRU cache Cache<K, V> with capacity 2: Get returns V or null and marks it recently used; Put inserts/updates and evicts the least recently used when full. Sequence: put(a,1) put(b,2) get(a)=1 put(c,3) [evicts b] get(b)=null get(c)=3 put(a,10) get(a)=10. Print each get as `get a -> 1` / `get b -> miss`, and `evicted b` when eviction happens.
