# Behavior Pack Metadata

Spark exports active Bedrock behavior-pack metadata through upstream `WorldStatistics.data_packs` (field 5).

## Current source

Endstone does not currently expose the active behavior-pack stack through its public plugin API. Spark therefore uses a filesystem compatibility fallback:

1. Resolve the active world from `server.properties` `level-name`, falling back to the Endstone level name when necessary.
2. Read only `worlds/<level>/world_behavior_packs.json` to determine the selected behavior-pack UUIDs and versions.
3. Resolve those references against `manifest.json` files beneath:
   - `worlds/<level>/behavior_packs` (`source = "world"`)
   - `behavior_packs` (`source = "server"`)
   - `development_behavior_packs` (`source = "server"`)
4. Export the manifest header name and description, the filesystem source, and `builtin = false`.

Only manifests containing a behavior module (`data` or `script`) are eligible. Installed but inactive packs are not exported. Missing or malformed manifests are skipped independently so one broken pack does not suppress other valid active packs.

## Resource-pack separation

Resource packs are deliberately excluded. Spark does not read `world_resource_packs.json`, does not scan `resource_packs`, and rejects resource-only manifests even if they are misplaced beneath a behavior-pack directory. Bedrock resource packs must not be represented as upstream `WorldStatistics.DataPack` entries.

## Runtime API migration

The filesystem view cannot reliably identify implicit/built-in packs and can only approximate the runtime-selected stack. When Endstone exposes a stable public API for the active behavior-pack stack, `EndstoneMetadataProvider` should prefer that API and map each selected pack directly to:

- `name`
- `description`
- `source` (`builtin`, `world`, `server`, or another runtime source)
- `builtin`

The filesystem reader should then remain only as a compatibility fallback for Endstone versions without the public API. Spark must not bind to Endstone's internal Bedrock `ResourcePackStack` ABI to obtain this metadata.
