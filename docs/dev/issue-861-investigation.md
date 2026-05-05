# Issue #861 geometry-validity investigation

Phase: 0 (investigation only)  
Branch: `cursor/issue-861-investigation-a4a9`  
Date: 2026-05-05

## 1. Issue #861 symptom

Fetched with:

```sh
gh issue view 861 --repo systemed/tilemaker --json number,title,body,comments,url,state,author,createdAt,updatedAt
```

Issue #861 is open, was reported by `GSYM18`, and is titled
`Faulty geometries (examples for ocean, water polygons, landcover) with openmaptiles configuration`.
The reporter says they used tilemaker's provided default OpenMapTiles files:

> To see if the issues are caused by our configurations, we used the provided
> default files for OMT (process-openmaptiles.lua and config-openmaptiles.json
> + shapefiles) and created the tiles for Bangladesh.

The reproducer quoted in the issue is tile-specific rather than a full command
line:

> Faulty ocean geometries:
> 3/5/3
> 4/11/7
> 5/23/14
> 7/95/56
>
> Faulty water geometries:
> Kaptai Lake: in zoom levels 6, 8 and from 12, the lake seems to be fine,
> but in zoom levels 7 (7/96/55), 9 (9/387/222), 10 (10/774/445),
> 11 (11/1548/891) the geometries are faulty.
>
> When briefly reviewing the OMT vector tiles for Europe, we also found the
> following "issues":
> Greece: tile 8/147/99, as well as faulty landcover geometry:
> In zoom level 9 and from 11 the geometry is fine, but in zoom level 10
> (tile: 10/588/385) a small part is "missing".

The discussion under #861 also notes the same class of artifacts with
VersaTiles and Greece. One comment for Kaptai Lake reports Mapbox GL JS'
browser-console error:

> Geometry exceeds allowed extent, reduce your vector tile buffer size

and asks whether Sutherland-Hodgman clipping could cause one-pixel edge cases.

### Classification

#861 matches the long-running clip/simplify/snap validity class rather than an
OSM relation-assembly-only bug:

* The reporter reproduced with the default OpenMapTiles config and shapefiles,
  and listed low/mid-zoom tile artifacts for ocean, water, and landcover.
* The failure is zoom/tile dependent: Kaptai Lake is OK at z6, z8, and z12+,
  but faulty at z7 and z9-z11. That pattern is characteristic of tile clipping,
  simplification, and final quantization rather than a single bad relation
  assembly result.
* The thread connects the symptoms to Greece/VersaTiles artifacts and MVT
  extent behavior; it does not isolate a malformed relation member ordering or
  missing-member assembly problem like the relation-specific class.

There is one important caveat: one maintainer comment says the Mapbox GL
`Geometry exceeds allowed extent` message can mean an undocumented client-side
geometry-size limit. That does not disprove the clip/snap class, but it means
the #861 Kaptai example should be validated with strict decoders and geometry
validators, not only with browser rendering.

## 2. #697 Flevoland reproduction on current master

Fetched #697 with:

```sh
gh issue view 697 --repo systemed/tilemaker --json number,title,body,comments,url,state,author,createdAt,updatedAt
```

#697's canonical reproducer describes long, narrow water polygons crossing tile
boundaries, and states:

> The affected OSM ways have in common that they cross tile boundaries and, are
> long and narrow polygons (rivers/canals). The artefacts occur when their narrow
> "tube" becomes a row of multiple small polygons due to simplification/snapping
> onto the grid of the vector tiles' internal coordinate system.

It also reports:

> MapLibre GL JS and OpenLayers render the polygon as expected.
> `tippecanoe-decode` crashes with `Polygon begins with an inner ring`.

and:

> What does not seem to have any influence:
> * building Tilemaker with LuaJIT or plain Lua
> * Boost 1.71 vs 1.74
> * simplification enabled/disabled in Tilemaker configuration
> * Tilemaker 2.4.0 vs. 3.0.0 vs. master (as of yesterday)

### Local environment

The cloud image did not initially contain all build/reproduction dependencies.
Installed packages for the investigation were:

```sh
sudo apt-get install -y \
  osmium-tool tippecanoe gdal-bin \
  build-essential libboost-dev libboost-filesystem-dev \
  libboost-program-options-dev libboost-system-dev lua5.1 liblua5.1-0-dev \
  libshp-dev libsqlite3-dev rapidjson-dev zlib1g-dev
```

The default `/usr/bin/c++` is Clang 18 and failed CMake's compiler test because
`-lstdc++` was not found via that driver. The investigation build used GCC:

```sh
cmake -S . -B build-gcc -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc --parallel 2
```

### Data preparation

The requested planet filtering was approximated with the allowed Geofabrik
Netherlands extract because downloading and filtering a full current planet is
unnecessarily large for Phase 0, and the task allowed "or use a Geofabrik NL
extract".

Commands:

```sh
curl -L --fail --retry 4 --retry-delay 4 \
  -o build-gcc/repro/issue-697/netherlands-latest.osm.pbf \
  https://download.geofabrik.de/europe/netherlands-latest.osm.pbf

osmium tags-filter \
  -o build-gcc/repro/issue-697/netherlands-water-polygons.osm.pbf \
  -v build-gcc/repro/issue-697/netherlands-latest.osm.pbf \
  a/natural=water a/waterway a/landuse=reservoir a/landuse=basin a/natural=glacier

osmium extract -b 5.32,52.29,5.92,52.68 --strategy smart \
  -o build-gcc/repro/issue-697/flevoland-water.osm.pbf \
  build-gcc/repro/issue-697/netherlands-water-polygons.osm.pbf
```

The #697 JSON/Lua snippets were used verbatim as temporary files under the
ignored build directory, then tilemaker was run as in the issue:

```sh
./build-gcc/tilemaker \
  --input build-gcc/repro/issue-697/flevoland-water.osm.pbf \
  --output build-gcc/repro/issue-697/polygon-test.mbtiles \
  --bbox 5.32,52.29,5.92,52.68 \
  --config build-gcc/repro/issue-697/config-polygon-test.json \
  --process build-gcc/repro/issue-697/process-polygon-test.lua
```

Tilemaker generated 1603 polygons and wrote z0-z14 tiles successfully.

### `tippecanoe-decode` results

Commands:

```sh
for z in 6 7 8; do
  tippecanoe-decode -Z "$z" -z "$z" \
    build-gcc/repro/issue-697/polygon-test.mbtiles \
    > "build-gcc/repro/issue-697/tippecanoe-z${z}.geojson" \
    2> "build-gcc/repro/issue-697/tippecanoe-z${z}.err"
done
```

Current results with the current Geofabrik extract:

| Zoom | `tippecanoe-decode` exit | `Polygon begins with an inner ring` | Other searched validity strings |
| ---- | ------------------------ | ----------------------------------- | ------------------------------- |
| z6   | 0                        | 0                                   | 0                               |
| z7   | 0                        | 0                                   | 0                               |
| z8   | 0                        | 0                                   | 0                               |

The exact #697 `tippecanoe-decode` crash is therefore not reproduced with the
current Netherlands extract. This is plausibly data-drift sensitive: the issue's
example feature `osm_id=1187867289` now resolves in the current Netherlands
extract to a way with bounding box `(5.6142576,51.8595155,5.627697,51.8618295)`,
outside the #697 Flevoland bbox.

### GDAL / GEOS round-trip results

Even though `tippecanoe-decode` no longer reports the original inner-ring-first
failure on current data, strict GDAL/GEOS processing still reproduces invalid
output from the same current-master pipeline.

Commands:

```sh
for z in 6 7 8; do
  ogr2ogr -f GeoJSON "build-gcc/repro/issue-697/ogr-z${z}.geojson" \
    build-gcc/repro/issue-697/polygon-test.mbtiles \
    -oo ZOOM_LEVEL="$z" \
    > "build-gcc/repro/issue-697/ogr-z${z}.out" \
    2> "build-gcc/repro/issue-697/ogr-z${z}.err"
done
```

Validity-related error counts:

| Zoom | `ogr2ogr` exit | `TopologyException` / validity lines |
| ---- | -------------- | ------------------------------------ |
| z6   | 0              | 8                                    |
| z7   | 0              | 16                                   |
| z8   | 0              | 16                                   |

Sample z8 errors:

```text
ERROR 1: TopologyException: side location conflict at 625675.29502830852 6878357.9735552268. This can occur if the input geometry is invalid.
ERROR 1: TopologyException: side location conflict at 621452.14921555261 6886727.8281524535. This can occur if the input geometry is invalid.
ERROR 1: TopologyException: side location conflict at 571978.2826579723 6883746.7840493312. This can occur if the input geometry is invalid.
ERROR 1: TopologyException: found non-noded intersection between LINESTRING (622771 6.884e+06, 622656 6.88392e+06) and LINESTRING (622694 6.88396e+06, 622924 6.88407e+06) at 622770.68795347167 6883995.2043912588
```

Conclusion: the exact `tippecanoe-decode` message from #697 is data-version
sensitive, but current master still emits invalid Flevoland water geometries
that strict GDAL/GEOS processing detects after decoding the generated MBTiles.

## 3. Current master geometry pipeline

### Top-level dispatch: `src/tilemaker.cpp`

* `main` (`src/tilemaker.cpp:90-566`) parses CLI options, resolves the
  clipping bbox, reads JSON config, creates the OSM and shapefile memory tile
  stores, reads PBF input, then schedules worker calls to `outputProc`.
* Bbox setup:
  * explicit `--bbox` parsing: `src/tilemaker.cpp:109-164`
  * PBF bbox fallback via `ReadPbfBoundingBox`: `src/tilemaker.cpp:145-150`
* PBF read orchestration:
  * `PbfProcessor::ReadPbfFile`: `src/tilemaker.cpp:291-326`
* Tile filtering and dispatch:
  * z6 bbox prefilter using `boost::geometry::within`:
    `src/tilemaker.cpp:365-379`
  * per-tile clipping-box intersection using `boost::geometry::intersects`:
    `src/tilemaker.cpp:430-439`
  * worker scheduling and `outputProc` call:
    `src/tilemaker.cpp:463-500`

### Per-tile processing and MVT emission: `src/tile_worker.cpp`

There is no `CheckNextObjectAndMerge` function in current master. The equivalent
merge path is in `ProcessObjects`.

* `outputProc` (`src/tile_worker.cpp:465-537`) creates a `TileBbox`, optionally
  copies features from an existing MBTiles tile, calls `ProcessLayer`, serializes
  the vtzero tile, and writes to MBTiles, PMTiles, or a filesystem tile.
* `ProcessLayer` (`src/tile_worker.cpp:390-457`) computes layer simplify/filter
  parameters, fetches per-layer objects, and calls `ProcessObjects`.
* `ProcessObjects` (`src/tile_worker.cpp:271-370`) builds the clipped geometry
  for each object with `source->buildWayGeometry`, then:
  * combines compatible points: `src/tile_worker.cpp:291-320`
  * appends and reorders compatible lines: `src/tile_worker.cpp:333-343`
  * combines compatible polygons with `union_many`: `src/tile_worker.cpp:345-355`
  * filters tiny polygon parts: `src/tile_worker.cpp:359-362`
  * calls `writeMultiLinestring` or `writeMultiPolygon`:
    `src/tile_worker.cpp:364-367`
* `writeMultiLinestring` (`src/tile_worker.cpp:96-172`) optionally simplifies
  lines, converts each point to MVT coordinates with `TileBbox::scaleLatpLon`,
  removes zero-length line segments, and emits vtzero line commands.
* `writeRing` (`src/tile_worker.cpp:174-206`) writes an already-scaled polygon
  ring to vtzero, removing consecutive duplicate points and dropping rings with
  fewer than four points.
* `writeMultiPolygon` (`src/tile_worker.cpp:209-269`) is the polygon output
  path. It first calls `bbox.scaleGeometry(mp)` at line 220, optionally
  simplifies the already-scaled integer-space geometry at lines 221-227, prints
  validity diagnostics only under `--verbose` at lines 232-240, then emits
  rings with `writeRing`.

### Per-tile clipping: `src/tile_data.cpp`

`TileDataSource::buildWayGeometry` (`src/tile_data.cpp:216-355`) is where
stored geometries are clipped for a tile:

* `LINESTRING_`: splits at clipping-box exits and runs `geom::intersection`
  against `bbox.getExtendBox()` (`src/tile_data.cpp:223-247`).
* `MULTILINESTRING_`: uses the clip cache and `geom::intersection`
  (`src/tile_data.cpp:250-267`).
* `POLYGON_`: uses the multipolygon clip cache, adjusts the bbox on end zoom,
  runs Sutherland-Hodgman `fast_clip`, then `geom::correct`, then validates. If
  the fast clip creates self-intersections or intersecting interiors, it falls
  back to `geom::intersection(input, box, output)` and `geom::correct(output)`
  (`src/tile_data.cpp:270-349`).

### Geometry primitives and helpers

`include/geom.h` contains the main geometry typedefs and helper declarations:

* Point/ring/polygon/multipolygon typedefs: `include/geom.h:30-39`
* self-intersection-aware simplifier declarations: `include/geom.h:43-46`
* in-place union helper `simplify_combine`: `include/geom.h:48-71`
* `namespace geom = boost::geometry`, `make_valid`, `union_many`, and
  `fast_clip` declarations: `include/geom.h:73-85`

`src/geom.cpp` contains:

* custom ring/polygon simplifier: `src/geom.cpp:15-115`
* Boost `simplify` for linestrings: `src/geom.cpp:117-121`
* custom multipolygon simplifier using `geom::correct` and `simplify_combine`:
  `src/geom.cpp:124-136`
* current `make_valid(MultiPolygon&)`, which delegates to vendored
  `geometry::correct`: `src/geom.cpp:138-145`
* pairwise `union_many`: `src/geom.cpp:150-169`
* Sutherland-Hodgman fast clipper: `src/geom.cpp:175-246`

`include/geometry/correct.hpp` is the vendored Kleunen correction/dissolve code:

* `geometry::impl::result_combine`: `include/geometry/correct.hpp:25-47`
* segment intersection discovery: `include/geometry/correct.hpp:91-137`
* invalid coordinate removal: `include/geometry/correct.hpp:139-152`
* ring orientation / closure / ring generation:
  `include/geometry/correct.hpp:154-290`
* inner/outer combining, odd-even alternative, and final `difference`:
  `include/geometry/correct.hpp:298-366`
* public `geometry::correct` overloads:
  `include/geometry/correct.hpp:389-430`

### Float-to-int conversion point

The exact final float-to-int conversion for polygons is:

* `TileBbox::scaleLatpLon` (`src/coordinates_geom.cpp:28-31`), which floors
  longitude/latp into tile-pixel coordinates.
* `TileBbox::scaleRing` (`src/coordinates_geom.cpp:34-51`), which applies
  `scaleLatpLon` and contains a local five-point duplicate/backtrack heuristic.
* `TileBbox::scaleGeometry` (`src/coordinates_geom.cpp:54-82`), which converts
  all rings of a `MultiPolygon` into integer-valued `Point`s.
* Call site for polygon output: `writeMultiPolygon` line
  `MultiPolygon current = bbox.scaleGeometry(mp);`
  (`src/tile_worker.cpp:209-220`).

This is the correct insertion point for a post-quantization validation/repair
stage: after `bbox.scaleGeometry(mp)` and after any integer-space simplification
currently performed by `writeMultiPolygon`, but before `writeRing` and vtzero
command emission.

For points and lines, the conversion is also in `src/tile_worker.cpp`:

* point output in `ProcessObjects`: `src/tile_worker.cpp:297-317`
* line output in `writeMultiLinestring`: `src/tile_worker.cpp:139-159`

### MVT command emission

There is no `src/vector_tile.cpp` in current master. MVT serialization is via
vendored vtzero builders:

* `vtzero::point_feature_builder`, `linestring_feature_builder`, and
  `polygon_feature_builder` are used in `src/tile_worker.cpp`.
* Tile serialization is `tile.serialize(outputdata)` in `outputProc`:
  `src/tile_worker.cpp:507-518` and `src/tile_worker.cpp:527-532`.
* vtzero command/delta writing lives under `include/vtzero/`, especially
  `include/vtzero/builder.hpp`.

### Output sinks

* `MBTiles::openForWriting`: `src/mbtiles.cpp:20-50`
* `MBTiles::insertOrReplace`: `src/mbtiles.cpp:58-65`
* `MBTiles::saveTile`: `src/mbtiles.cpp:82-93`
* `MBTiles::readTileAndUncompress` for merge mode:
  `src/mbtiles.cpp:127-150`
* `PMTiles::open`: `src/pmtiles.cpp:16-22`
* `PMTiles::close`: `src/pmtiles.cpp:24-87`
* `PMTiles::saveTile`: `src/pmtiles.cpp:135-173`

### OSM relation assembly

Current master does not have `src/read_pbf.*`; PBF reading is in
`src/pbf_reader.cpp`, `include/pbf_reader.h`, and `src/pbf_processor.cpp`.

Relation pipeline:

* `PbfProcessor::ScanRelations` marks used relations/ways and records relation
  members for Lua relation reads (`src/pbf_processor.cpp:201-258`).
* `PbfProcessor::ReadRelations` splits relation ways into `outerWayVec` and
  `innerWayVec` and calls `output.setRelation`
  (`src/pbf_processor.cpp:261-345`).
* Read phase order is RelationScan, optional WayScan, Nodes, Ways, Relations:
  `src/pbf_processor.cpp:609-617`.
* `OsmLuaProcessing::setRelation` invokes Lua, then stores either
  `multiPolygonCached()` or `multiLinestringCached()` in `osmMemTiles`
  (`src/osm_lua_processing.cpp:1143-1193`).
* `OSMStore::wayListMultiPolygon` joins outer/inner member ways, assigns inner
  rings to outers with `geom::within`, and calls `geom::correct(mp)`
  (`src/osm_store.cpp:63-94`).
* `OSMStore::mergeMultiPolygonWays` is the endpoint-joiner for member ways
  (`src/osm_store.cpp:115-225`).
* Simple closed ways are converted with `llListPolygon` and
  `boost::geometry::correct(poly)` in `include/osm_store.h:332-337`.

## 4. Geometry operation call sites

This list was produced by searching current master for the relevant Boost
Geometry calls, including calls through the local `geom` alias.

### `bg::correct` / `geom::correct`

| File | Lines | Context |
| ---- | ----- | ------- |
| `src/tile_data.cpp` | 331, 340 | after fast clip and after Boost intersection fallback |
| `src/geojson_processor.cpp` | 131, 167 | GeoJSON polygon/multipolygon ingest |
| `src/osm_store.cpp` | 93 | relation multipolygon assembly winding |
| `src/geom.cpp` | 130 | after custom polygon simplification |
| `include/osm_store.h` | 336 | simple way polygon conversion |
| `include/shp_mem_tiles.h` | 60 | indexed shapefile area aggregation |

The vendored `geometry::correct` is called by `make_valid(MultiPolygon&)` in
`src/geom.cpp:138-145`.

### `bg::is_valid` / `geom::is_valid`

| File | Lines | Context |
| ---- | ----- | ------- |
| `include/geometry/correct.hpp` | 147 | point validity in vendored correction |
| `src/tile_data.cpp` | 333 | post-clip polygon validation |
| `src/shp_processor.cpp` | 259, 263 | shapefile validity before/after `make_valid` |
| `src/tile_worker.cpp` | 233, 236 | verbose-only output polygon diagnostics |
| `include/osm_lua_processing.h` | 174, 176 | Lua way/relation geometry validation |

### `bg::union_` / `geom::union_`

| File | Lines | Context |
| ---- | ----- | ------- |
| `src/geom.cpp` | 164 | `union_many` |
| `include/geometry/correct.hpp` | 37 | vendored `result_combine` |
| `include/geom.h` | 61 | `simplify_combine` |
| `include/shp_mem_tiles.h` | 57 | `AreaIntersecting` aggregation |

### `bg::intersection` / `geom::intersection`

| File | Lines | Context |
| ---- | ----- | ------- |
| `include/geometry/correct.hpp` | 116 | segment intersection discovery |
| `src/tile_data.cpp` | 246, 265, 339 | line/multiline clipping and polygon fallback clipping |
| `src/shp_processor.cpp` | 211, 270 | shapefile line/polygon clipping |
| `src/geojson_processor.cpp` | 122, 133, 156, 169 | GeoJSON line/polygon clipping |
| `src/osm_lua_processing.cpp` | 475 | Lua spatial intersection with indexed shapefile polygons |
| `src/osm_mem_tiles.cpp` | 77 | materialized way line clipping |

### `bg::difference`

| File | Lines | Context |
| ---- | ----- | ------- |
| `include/geometry/correct.hpp` | 365 | subtract combined inners from combined outers |

### `bg::simplify`

| File | Lines | Context |
| ---- | ----- | ------- |
| `src/geom.cpp` | 120 | linestring simplification |

Additional local simplifier call sites:

* `writeMultiLinestring`: `src/tile_worker.cpp:117-124`
* `writeMultiPolygon`: `src/tile_worker.cpp:221-226`
* Visvalingam multipolygon simplification calls `make_valid(output)`:
  `src/visvalingam.cpp:261-264`

## 5. Phase 0 conclusion

The root-cause hypothesis is consistent with current master:

1. Relation assembly has validation/correction, and #861 is not isolated to a
   single relation.
2. Per-tile polygon clipping validates after `fast_clip` and can fall back to
   `geom::intersection`, but this happens before final tile-pixel quantization.
3. The final polygon float-to-int conversion is `TileBbox::scaleGeometry` in
   `writeMultiPolygon`; no mandatory validation/repair occurs after it.
4. Current master still emits Flevoland water geometries that strict GDAL/GEOS
   processing reports as invalid after MBTiles output, even though the exact
   #697 `tippecanoe-decode` inner-ring-first failure was not reproduced with the
   current Netherlands extract.

Proceeding to Phase 1 is reasonable only if the regression fixture is pinned to
data that fails deterministically on master. The current live Geofabrik
Netherlands extract is sufficient to demonstrate GDAL/GEOS topology errors, but
it is not sufficient for the exact #697 `tippecanoe-decode` error text.
