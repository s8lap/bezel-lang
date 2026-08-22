type Handler<T> := (T, string?) -> bool;
type Lookup<K, V> := Dictionary<K, Array<V>>;
type Predicate<T> := (T) -> bool;
type Reducer<T, TAccum> := (TAccum, T) -> TAccum;
