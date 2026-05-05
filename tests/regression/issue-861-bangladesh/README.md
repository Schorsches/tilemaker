# Issue #861 Bangladesh Kaptai fixture

This fixture is a minimized extract of OSM relation `1626722` (Kaptai Lake)
from the Geofabrik Bangladesh extract used during the issue #861 investigation.
It is small enough to keep in the repository and still reproduces the strict
MVT ring-order failure on current master with the default OpenMapTiles config.

The fixture was generated with:

```sh
osmium getid -r -t bangladesh-latest.osm.pbf r1626722 \
  -o kaptai.osm.pbf
```

Validation targets:

* `7/96/55`
* `9/387/222`
* `10/774/445`
* `11/1548/891`

On current master, each fails `tippecanoe-decode` with:

```text
Polygon begins with an inner ring
```

Control tile:

* `12/3097/1783`

The control tile decodes cleanly on current master and guards against treating
all Kaptai tiles as invalid by construction.
