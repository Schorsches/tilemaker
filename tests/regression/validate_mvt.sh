#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

TILEMAKER_BIN="${TILEMAKER_BIN:-${REPO_ROOT}/build/tilemaker}"
WORK_DIR="${WORK_DIR:-${REPO_ROOT}/build/regression}"
CHECKER_BIN="${WORK_DIR}/mvt_geometry_check"

mkdir -p "${WORK_DIR}"

if [[ ! -x "${TILEMAKER_BIN}" ]]; then
	echo "tilemaker binary not found or not executable: ${TILEMAKER_BIN}" >&2
	exit 2
fi

require_tool() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "required tool not found: $1" >&2
		exit 2
	fi
}

require_tool sqlite3
require_tool tippecanoe-decode
require_tool grep

compile_checker() {
	"${CXX:-g++}" -std=c++17 -O2 \
		-I"${REPO_ROOT}/include" \
		"${SCRIPT_DIR}/mvt_geometry_check.cpp" \
		-lsqlite3 -lz \
		-o "${CHECKER_BIN}"
}

ensure_omt_shapefiles() {
	if [[ ! -f "${REPO_ROOT}/coastline/water_polygons.shp" ]]; then
		"${REPO_ROOT}/get-coastline.sh"
	fi
	if [[ ! -f "${REPO_ROOT}/landcover/ne_10m_urban_areas/ne_10m_urban_areas.shp" ]]; then
		"${REPO_ROOT}/get-landcover.sh"
	fi
}

run_tippecanoe_check() {
	local mbtiles="$1"
	local z="$2"
	local x="$3"
	local y="$4"
	local label="$5"
	local out="${WORK_DIR}/${label}.geojson"
	local err="${WORK_DIR}/${label}.err"

	if ! tippecanoe-decode "${mbtiles}" "${z}" "${x}" "${y}" >"${out}" 2>"${err}"; then
		echo "tippecanoe-decode failed for ${label} (${z}/${x}/${y})" >&2
		if grep -Fq "Polygon begins with an inner ring" "${err}"; then
			echo "found: Polygon begins with an inner ring" >&2
		fi
		return 1
	fi

	if grep -Eiq "Polygon begins with an inner ring|zero-length|invalid|self-intersection|topology|error" "${err}" "${out}"; then
		echo "tippecanoe-decode reported validity text for ${label} (${z}/${x}/${y})" >&2
		return 1
	fi
}

run_checker() {
	local mbtiles="$1"
	local z="$2"
	local x="$3"
	local y="$4"
	local layer="$5"
	local label="$6"

	"${CHECKER_BIN}" "${mbtiles}" "${z}" "${x}" "${y}" "${layer}" \
		>"${WORK_DIR}/${label}-checker.out" \
		2>"${WORK_DIR}/${label}-checker.err"
}

run_issue_861_bangladesh() {
	ensure_omt_shapefiles

	local fixture="${SCRIPT_DIR}/issue-861-bangladesh/kaptai.osm.pbf"
	(cd "${SCRIPT_DIR}/issue-861-bangladesh" && sha256sum -c kaptai.osm.pbf.sha256)

	local output="${WORK_DIR}/issue-861-bangladesh.mbtiles"
	rm -f "${output}"

	"${TILEMAKER_BIN}" \
		--input "${fixture}" \
		--output "${output}" \
		--bbox 92.0,22.4,92.4,23.1 \
		--config "${REPO_ROOT}/resources/config-openmaptiles.json" \
		--process "${REPO_ROOT}/resources/process-openmaptiles.lua" \
		>"${WORK_DIR}/issue-861-bangladesh.tilemaker.out" \
		2>"${WORK_DIR}/issue-861-bangladesh.tilemaker.err"

	local failed=0
	for tile in \
		"kaptai-z7:7:96:55" \
		"kaptai-z9:9:387:222" \
		"kaptai-z10:10:774:445" \
		"kaptai-z11:11:1548:891"
	do
		IFS=: read -r label z x y <<<"${tile}"
		run_tippecanoe_check "${output}" "${z}" "${x}" "${y}" "${label}" || failed=1
		run_checker "${output}" "${z}" "${x}" "${y}" "water" "${label}" || failed=1
	done

	# This control tile was visually reported as OK and decodes cleanly today.
	# Keep it as a decoder control only; strict reconstructed-Boost validity is
	# tracked on the reported failing tiles above.
	run_tippecanoe_check "${output}" 12 3097 1783 "kaptai-z12-control" || failed=1

	return "${failed}"
}

run_issue_697_flevoland() {
	if [[ "${RUN_FLEVOLAND:-0}" != "1" ]]; then
		echo "Skipping Flevoland fixture; set RUN_FLEVOLAND=1 to enable"
		return 0
	fi

	require_tool ogr2ogr

	local fixture="${SCRIPT_DIR}/issue-697-flevoland/flevoland-water.osm.pbf"
	(cd "${SCRIPT_DIR}/issue-697-flevoland" && sha256sum -c flevoland-water.osm.pbf.sha256)

	local output="${WORK_DIR}/issue-697-flevoland.mbtiles"
	rm -f "${output}"

	"${TILEMAKER_BIN}" \
		--input "${fixture}" \
		--output "${output}" \
		--bbox 5.32,52.29,5.92,52.68 \
		--config "${SCRIPT_DIR}/issue-697-flevoland/config-polygon-test.json" \
		--process "${SCRIPT_DIR}/issue-697-flevoland/process-polygon-test.lua" \
		>"${WORK_DIR}/issue-697-flevoland.tilemaker.out" \
		2>"${WORK_DIR}/issue-697-flevoland.tilemaker.err"

	local failed=0
	for z in 6 7 8; do
		local err="${WORK_DIR}/issue-697-flevoland-z${z}.ogr.err"
		ogr2ogr -f GeoJSON "${WORK_DIR}/issue-697-flevoland-z${z}.geojson" \
			"${output}" -oo ZOOM_LEVEL="${z}" \
			>"${WORK_DIR}/issue-697-flevoland-z${z}.ogr.out" \
			2>"${err}" || failed=1

		if grep -Eiq "TopologyException|Self-intersection|Ring Self-intersection|invalid|warning|error" "${err}"; then
			echo "GDAL/GEOS reported invalid Flevoland geometry at z${z}" >&2
			failed=1
		fi
	done

	return "${failed}"
}

main() {
	compile_checker

	local failed=0
	run_issue_861_bangladesh || failed=1
	run_issue_697_flevoland || failed=1

	if [[ "${failed}" -ne 0 ]]; then
		echo "MVT regression validation failed. Logs are in ${WORK_DIR}" >&2
		return 1
	fi

	echo "MVT regression validation passed"
}

main "$@"
