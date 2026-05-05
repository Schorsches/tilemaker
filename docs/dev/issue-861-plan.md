# Issue #861 implementation plan

Phase: 2 (plan before patching)  
Branch: `cursor/issue-861-investigation-a4a9`  
Date: 2026-05-05

## Goals

Fix the invalid MVT polygon emission class confirmed by:

* #861 Bangladesh/default OpenMapTiles tiles failing `tippecanoe-decode` with
  `Polygon begins with an inner ring`.
* #697 Flevoland water output still producing strict GDAL/GEOS topology errors.

The plan keeps Boost.Geometry as the default path, keeps GEOS optional, and does
not change the Lua API or existing JSON config schema.

## Non-goals

* Do not make GEOS mandatory.
* Do not replace the existing relation assembler as the first fix.
* Do not add unbounded polygon repair throughout the ingestion pipeline.
* Do not rely on Boost.Geometry extension algorithms that are not stable public
  API.
* Do not silently drop geometry. Any final-stage skipped features must be counted
  and reported.

## Current insertion points

Phase 0 identified the most efficient high-value insertion point:

* Float-to-int conversion:
  * `TileBbox::scaleLatpLon` (`src/coordinates_geom.cpp:28-31`)
  * `TileBbox::scaleRing` (`src/coordinates_geom.cpp:34-51`)
  * `TileBbox::scaleGeometry` (`src/coordinates_geom.cpp:54-82`)
* Polygon output call site:
  * `writeMultiPolygon` (`src/tile_worker.cpp:209-269`)
  * currently starts with `MultiPolygon current = bbox.scaleGeometry(mp);`
* MVT emission:
  * `writeRing` (`src/tile_worker.cpp:174-206`)
  * vtzero `polygon_feature_builder`

The first production change should run validation/repair after the final
tile-pixel conversion and before `writeRing`.

## Runtime control

Add a new runtime flag:

```text
--validate-geometry={off,fast,strict}
```

Semantics:

* `off`: current behavior, no post-quantization repair and no drop-on-fail.
* `fast`: default. Boost-only final-stage validation/repair. Drop only if still
  invalid after repair.
* `strict`: GEOS-backed final-stage repair. Requires a build configured with
  `TILEMAKER_USE_GEOS=ON`; otherwise fail fast during option parsing with a clear
  error.

Implementation notes:

* Add an enum to `OptionsParser::Options`, parse with
  `boost::program_options`, and propagate into `SharedData`.
* Keep Lua and JSON config unchanged.
* Preserve command-line compatibility: existing commands remain valid. The new
  default changes output validity, not accepted inputs or config syntax. Users
  needing byte-for-byte legacy behavior can specify `--validate-geometry=off`.

## Build control

Add an optional CMake dependency:

```cmake
option(TILEMAKER_USE_GEOS "Build with GEOS for robust geometry validation" OFF)
```

When enabled:

* Prefer `find_package(GEOS 3.10 CONFIG)`.
* If the CMake config package is unavailable, fall back to a local
  `FindGEOS.cmake` only if needed by supported platforms.
* Link GEOS only to `tilemaker`, not `tilemaker-server`.
* Add `target_compile_definitions(tilemaker PRIVATE TILEMAKER_USE_GEOS=1)`.

When disabled:

* No GEOS headers are included.
* No GEOS libraries are linked.
* `strict` mode is unavailable and errors clearly.

## Shared statistics and logging

Add worker-safe counters to `SharedData` or a small contained stats struct:

* polygon features repaired by Boost fast path
* polygon features repaired by GEOS strict path
* polygon features skipped after failed validation
* rings removed because they collapsed below the MVT minimum

Use atomics to avoid adding contention to hot tile writes. Print one summary at
the end of `main`, after worker completion and before the final success line.
Under `--verbose`, include feature identifiers/layers for skipped polygon
features when available.

## Stage A: final-stage make_valid and MVT winding

### Scope

Patch only the output side:

* `include/geometry/make_valid.hpp` or `include/geometry/validity.hpp`
* `src/tile_worker.cpp`
* `include/options_parser.h`
* `src/options_parser.cpp`
* `include/shared_data.h`
* `src/tilemaker.cpp`
* `CMakeLists.txt`
* focused tests/harness updates from Phase 1

### Algorithm

In `writeMultiPolygon`:

1. Convert source geometry to tile-pixel coordinates:
   `MultiPolygon current = bbox.scaleGeometry(mp);`
2. Apply existing integer-space simplification if configured.
3. If validation mode is not `off`:
   * remove spikes
   * run final-stage make-valid
   * validate the result
   * enforce MVT ring winding
   * validate ring sizes before emission
4. If still invalid, skip the feature, increment counters, and log under
   `--verbose`.
5. Emit rings with vtzero.

### MVT winding

Implement `enforce_mvt_winding(MultiPolygon&)`.

Rules in tile-pixel y-down coordinates:

* exterior rings must have positive signed area for MVT screen-space winding
  (clockwise on screen)
* interior rings must have negative signed area

Use a small local signed-area helper over integer-valued `Point`s rather than
relying on OGC orientation from `bg::correct`, because Boost's normal polygon
orientation is not the MVT orientation after y-axis inversion.

Call ordering:

```text
scale/simplify -> make_valid -> enforce_mvt_winding -> writeRing
```

### Boost-only fast path

Use a conservative fallback with bounded work:

1. Split `MultiPolygon` into individual `Polygon`s.
2. For each polygon:
   * run `geom::remove_spikes`
   * run the existing `make_valid(MultiPolygon&)` based on vendored
     `geometry::correct`
   * if needed, attempt Boost `buffer` with zero distance using
     `distance_symmetric<double>(0)`, `side_straight`, `join_miter`,
     `end_flat`, and `point_square`
   * run `geom::correct`
   * keep only valid, non-empty polygons
3. Reassemble and validate the multipolygon.

The buffer(0) attempt is not the only repair; it is a bounded fallback after the
existing kleunen correction. If a ring still fails, the final feature is skipped
and counted rather than emitted invalidly.

### GEOS strict path

When `TILEMAKER_USE_GEOS=ON` and `--validate-geometry=strict`:

* Use the GEOS reentrant C API.
* Keep one `GEOSContextHandle_t` per worker thread (`thread_local` RAII wrapper).
* Convert tile-pixel `MultiPolygon` to GEOS through WKB or direct coordinate
  sequence construction.
* Prefer `GEOSMakeValidWithParams_r` with
  `GEOS_MAKE_VALID_STRUCTURE`.
* Round-trip only polygonal components back into tilemaker `MultiPolygon`.
* Run `enforce_mvt_winding` after GEOS repair.

Direct coordinate sequence construction is likely faster than WKB and avoids a
serialization dependency, but WKB is acceptable if simpler and covered by the
regression harness.

### Acceptance

* Bangladesh #861 reported tiles listed in Phase 0 decode with zero
  `Polygon begins with an inner ring` failures in `fast` mode.
* Same Bangladesh tiles decode cleanly in `strict` mode when GEOS is enabled.
* Flevoland fixture has zero `tippecanoe-decode` strict ring-order failures and
  zero GDAL/GEOS topology errors for the tested z6-z8 range.
* Unit or regression validation confirms MVT exterior/interior winding after
  quantization.
* No Lua API or JSON config changes.
* Default build works without GEOS.

## Stage B: integer snap before clipping

### Scope

Move the dominant root-cause operation earlier for polygons:

* `src/tile_data.cpp`
* `src/coordinates_geom.cpp` / `include/coordinates_geom.h`
* `src/geom.cpp` / `include/geom.h`

### Plan

1. Add helpers to project geometry into tile-pixel integer space before the
   tile-bbox clip.
2. Clip in the same coordinate space used for emission.
3. Run Stage A final validation and MVT winding after clipping.
4. Keep the existing floating-point clipping path available behind a runtime
   clip implementation flag if needed for rollback:

```text
--clip-impl={current,int}
```

Default after Stage B should become `int` only after Stage A/B fixtures pass and
performance is within budget. If the diff is too invasive, land Stage B as an
experimental option first and switch default in a follow-up commit.

### Optional wagyu path

Evaluate `mapbox/wagyu` only if integer-space Boost clipping plus Stage A still
leaves invalid Saimaa/Boston-class residue:

```text
--clip-impl={current,int,wagyu}
```

Wagyu is header-only, but it still adds maintenance surface. Do not add it unless
the regression data proves current Boost/int clipping is insufficient.

### Acceptance

* Saimaa stress fixture improves or passes.
* Bangladesh #861 and Flevoland remain green.
* Stage B wall-clock regression over Stage A is within 5% on the agreed
  benchmark extract.

## Stage C: optional OverlayNG-backed polygon combining

### Scope

Target only polygon merge/union hot spots that can create invalid output:

* `union_many` (`src/geom.cpp:150-169`)
* `ProcessObjects` polygon combine path (`src/tile_worker.cpp:345-355`)
* `simplify_combine` (`include/geom.h:48-71`) only if Stage A/B evidence points
  there

### Plan

When GEOS is enabled, provide a strict union path using GEOS OverlayNG-backed
union operations. Keep Boost as the default in non-GEOS builds.

Runtime control:

```text
--polygon-union={boost,geos}
```

Default remains `boost` unless benchmark and regression evidence show the GEOS
path is both safer and within budget. This avoids paying the GEOS conversion cost
for all users and keeps Stage C independently reversible.

### Acceptance

* Combine-heavy fixtures produce no invalid output.
* Combine-heavy performance is no worse than 1.3x current Boost behavior unless
  explicitly opted into strict mode.

## Stage D: documentation, changelog, and cleanup

Update:

* `README.md`
* `docs/geometry-validity.md`
* `docs/man/tilemaker.1`
* `CHANGELOG.md`

Document:

* `--validate-geometry`
* optional `TILEMAKER_USE_GEOS`
* MVT winding expectations
* why validation happens after final quantization
* how to reproduce #861 and #697
* trade-offs for `fast` vs `strict`

## Regression harness dependency

Do not begin Stage A production code until Phase 1 lands a red-on-master
regression harness.

Preferred fixtures:

1. Bangladesh/default OMT fixture based on the current Geofabrik extract or a
   minimized pinned PBF derived from it, because it reproduces the exact #861
   `tippecanoe-decode` failure deterministically today.
2. Flevoland water fixture for GDAL/GEOS topology errors, because the exact #697
   `tippecanoe-decode` text is data-version sensitive on the live Geofabrik
   Netherlands extract.
3. Saimaa relation fixture as stress coverage, initially expected-fail if needed.

The harness should report:

* exact failing tile coordinate
* layer
* decoder or validator error
* feature id when available

## Performance budget

Stage A must keep planet-scale build wall-clock regression at or below 15% versus
current master on a Geofabrik Germany extract.

Measurement method:

1. Build Release with the same compiler and dependency set.
2. Run current master and Stage A with identical command lines, input extract,
   config, process file, shapefile resources, thread count, and output mode.
3. Run at least two warm-cache measurements per variant.
4. Compare the best successful run for each variant, and record:
   * wall clock
   * peak RSS when available
   * number of repaired/skipped features
   * output tile count

Optimization guardrails:

* Only validate polygon features after quantization.
* Skip expensive repair when `geom::is_valid` already succeeds and ring winding
  is correct.
* Avoid global locks in hot paths; use thread-local GEOS contexts and atomic
  counters.
* Do not round-trip through GEOS unless `strict` mode is requested.

## Rollback plan

Every stage is independently reversible:

* Stage A:
  * `--validate-geometry=off` restores legacy output behavior.
  * GEOS code is compile-time gated by `TILEMAKER_USE_GEOS`.
* Stage B:
  * `--clip-impl=current` restores current floating-point clip behavior.
* Stage C:
  * `--polygon-union=boost` restores current Boost union behavior.

If Stage A fast mode drops more than 0.1% of Flevoland fixture polygon features,
stop and investigate before continuing. If the Germany benchmark exceeds the
15% wall-clock budget, keep `fast` available but reconsider the default before
merging.

## Proposed commit sequence

1. Phase 1 regression harness (red on master).
2. Stage A CLI/build plumbing and stats only.
3. Stage A Boost fast validation/winding implementation.
4. Stage A optional GEOS strict implementation.
5. Stage A documentation and benchmark results.
6. Stage B integer clipping experiment behind flag.
7. Stage B default switch only after fixtures and benchmark pass.
8. Stage C optional GEOS union only if Stage A/B leave validated residue.

This keeps each review unit small: first the harness, then runtime plumbing, then
Boost-only behavior, then optional GEOS, then deeper clipping/overlay changes.
