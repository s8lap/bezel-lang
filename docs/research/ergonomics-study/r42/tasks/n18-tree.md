# n18-tree

*Area:* borrows

A file-system-like tree: `Node` owns its children (`OwningArray<Node>`) and holds a plain back-pointer to its parent (`Node?`). Build `/ -> usr -> bin, lib` and `/ -> etc`. Print each node's full path from the parent chain (`/usr/bin`), the depth-first listing with indentation, and `leaves 3`. Record the trust-report lines.
