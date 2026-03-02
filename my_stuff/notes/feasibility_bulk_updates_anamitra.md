# Feasibility note: bulk pointer-relation updates in SVF

This note summarizes why SVF currently performs points-to updates mostly at variable granularity, and what would be required to support map-level bulk updates safely for flow-sensitive and context-sensitive analyses.

## Current API shape and solver coupling

- The abstract points-to interface (`PTData`) exposes updates keyed by a destination key (`unionPts(dst, src)` or `unionPts(dst, set)`), not map-to-map operations.

- Flow-sensitive APIs (`DFPTData`) are similarly key-scoped (`updateDFInFromIn`, `updateDFInFromOut`, `updateDFOutFromIn`, and `updateAllDFOutFromIn` iterating variable-wise).

- Context-sensitive demand-driven analysis (`CondPTAImpl`) also routes updates through `unionPts(CVar, CPtSet)` and related per-variable routines.

As a result, worklist scheduling and "changed" tracking are tied to key-level updates.

## Why key-level updates are important today

### 1) Change tracking is per variable (and per program point)

- Incremental DFPT (`MutableIncDFPTData`) tracks whether a specific variable at a specific location has new IN/OUT facts, and uses that to gate propagation.

- Methods like `updateDFOutFromIn` clear and set update bits for individual variables.

A naive map-level `unionPts(map1, map2)` that does not preserve these fine-grained flags would either over-propagate or miss propagation.

### 2) Flow-sensitive propagation is edge/variable selective

- In `FlowSensitive::propAlongIndirectEdge`, each edge carries a restricted points-to object set, and propagation iterates object-by-object (including field-insensitive expansion).

-  `propVarPtsFromSrcToDst` then updates a specific `(loc,var)` pair through `updateInFromIn` / `updateInFromOut`.

Bulk map union would need to preserve this selectivity; otherwise precision and performance both regress.

### 3) Versioned flow-sensitive analysis depends on object-version pairs

-  `VersionedFlowSensitive` updates versioned keys such as `atKey(o, v)` and triggers dependent version propagation when a specific pair changes.

- Worklist pushes are driven by changes to these specific versioned variables.

Map-level union is possible only if it still reports the exact changed keys so dependent version propagation remains sound and efficient.

### 4) Context-sensitive DDA computes one queried variable at a time

-  `ContextDDA::computeDDAPts` and `FlowDDA::computeDDAPts` run demand-driven from one query and cache/union that query's result.

- This algorithmic structure is query-centric, not whole-map transfer-centric.

## Feasibility assessment

-  **Semantically feasible**: yes, but only as an internal optimization layer, not as a simple API replacement.

-  **Low-risk route**: keep existing key-level API and add a bulk helper that:

1. iterates candidate keys,

2. performs per-key union with existing routines,

3. records exactly which keys changed,

4. updates per-key/per-loc incremental flags exactly as today.

-  **High-risk route**: replacing key APIs with map-to-map APIs directly across flow/context-sensitive solvers would require redesigning:

- change-bit bookkeeping,

- worklist triggering contracts,

- strong-update handling,

- version-reliance propagation,

- sparse edge filtering.

## Practical recommendation

1. Start with **batched wrappers** (e.g., `unionPtsBatch(vector<pair<Key,DataSet>>)`), not whole-map replacement.

2. Maintain exact changed-key output from batch operations.

3. Keep edge-local/object-local filtering in `FlowSensitive` and `VersionedFlowSensitive`.

4. Benchmark on large suites to check whether batching reduces overhead versus current bitset unions (which are already efficient).

In short: bulk updates are plausible, but only with strict preservation of the current fine-grained change semantics.