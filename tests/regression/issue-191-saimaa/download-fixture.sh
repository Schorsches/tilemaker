#!/usr/bin/env bash

set -o errexit
set -o pipefail
set -o nounset

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${WORK_DIR:-${SCRIPT_DIR}/.work}"
OUTPUT="${SCRIPT_DIR}/saimaa-7379046.osm.pbf"

mkdir -p "${WORK_DIR}"

if [[ ! -f "${WORK_DIR}/finland-latest.osm.pbf" ]]; then
	curl -L --fail --retry 4 --retry-delay 4 \
		-o "${WORK_DIR}/finland-latest.osm.pbf" \
		https://download.geofabrik.de/europe/finland-latest.osm.pbf
fi

osmium getid -r -t \
	"${WORK_DIR}/finland-latest.osm.pbf" \
	r7379046 \
	-o "${OUTPUT}" \
	--overwrite

(cd "${SCRIPT_DIR}" && sha256sum "$(basename "${OUTPUT}")" > "$(basename "${OUTPUT}").sha256")
