using var a := new Buffer();          // new T(...)            -> owned
var b := source.OpenBuffer();         // returns owned Buffer   -> owned
var c := source.Buffer;               // field / property read  -> borrowed (source: source)
var d := registry.Find("key");        // returns Buffer (no owned) -> borrowed
var e := move holder.slot;            // move out of owned field -> owned
