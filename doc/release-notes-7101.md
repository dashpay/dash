## Indexes

### Async Index Migration (#7101)

`TimestampIndex`, `SpentIndex`, and `AddressIndex` have been migrated from synchronous to asynchronous operation, following the same pattern as `TxIndex` and other indexes using `BaseIndex` framework.

When enabling an index for the first time, the node will build the index in the background while remaining fully operational. Progress can be monitored via the `getindexinfo` RPC.

Existing nodes with indexes enabled will automatically migrate data from the old location (block index database) to new separate databases on first startup. This migration may take 20-40 minutes or longer depending on hardware specifications and index sizes. The node will log progress during migration.

Breaking changes:
- `SpentIndex` and `AddressIndex` are incompatible with pruned nodes as they require access to undo data now
