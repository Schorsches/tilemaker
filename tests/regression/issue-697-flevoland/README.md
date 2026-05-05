# Issue #697 Flevoland fixture

This directory is reserved for the Flevoland water-polygon regression described
in issue #697.

The Phase 0 investigation showed that the current live Geofabrik Netherlands
extract no longer reproduces the exact `tippecanoe-decode` message from the
original report, but still produces GDAL/GEOS topology errors at z6-z8. The
Bangladesh/Kaptai fixture is therefore the deterministic red-on-master fixture
used by the initial harness.

Before enabling this fixture in CI, pin either:

* a minimized PBF derived from source data that reproduces the original
  `Polygon begins with an inner ring` error, or
* a downloaded extract plus checksum that reliably reproduces the GDAL/GEOS
  topology errors.
