#ifndef TILEMAKER_GEOMETRY_MAKE_VALID_HPP
#define TILEMAKER_GEOMETRY_MAKE_VALID_HPP

#include "geom.h"

#include <algorithm>
#include <cmath>

namespace tilemaker {
namespace geometry {

enum class MakeValidStrategy {
	Boost,
	Geos
};

struct MakeValidResult {
	bool changed = false;
	bool valid = true;
	bool usedGeos = false;
};

inline double signed_area(const Ring& ring) {
	if (ring.size() < 4) {
		return 0.0;
	}

	double area = 0.0;
	for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
		area += ring[i].x() * ring[i + 1].y() - ring[i + 1].x() * ring[i].y();
	}
	return area / 2.0;
}

inline void enforce_mvt_winding(MultiPolygon& mp) {
	for (auto& polygon : mp) {
		Ring& outer = polygon.outer();
		if (signed_area(outer) < 0.0) {
			std::reverse(outer.begin(), outer.end());
		}

		for (auto& inner : polygon.inners()) {
			if (signed_area(inner) > 0.0) {
				std::reverse(inner.begin(), inner.end());
			}
		}
	}
}

inline std::size_t remove_collapsed_rings(MultiPolygon& mp) {
	std::size_t removed = 0;
	for (auto& polygon : mp) {
		auto& inners = polygon.inners();
		const auto before = inners.size();
		inners.erase(std::remove_if(
			inners.begin(),
			inners.end(),
			[](const Ring& ring) {
				return ring.size() < 4 || signed_area(ring) == 0.0;
			}),
			inners.end());
		removed += before - inners.size();
	}

	const auto before = mp.size();
	mp.erase(std::remove_if(
		mp.begin(),
		mp.end(),
		[](const Polygon& polygon) {
			return polygon.outer().size() < 4 || signed_area(polygon.outer()) == 0.0;
		}),
		mp.end());
	removed += before - mp.size();
	return removed;
}

inline MakeValidResult make_valid(MultiPolygon& mp, MakeValidStrategy strategy) {
	MakeValidResult result;
	if (strategy == MakeValidStrategy::Geos) {
#ifdef TILEMAKER_USE_GEOS
		result.usedGeos = true;
		// GEOS strict repair is wired in a later Stage A commit. Until then,
		// fall through to the Boost path so the default build remains usable.
#else
		result.valid = false;
		return result;
#endif
	}

	geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
	if (geom::is_valid(mp, failure)) {
		return result;
	}

	MultiPolygon repaired = mp;
	geom::remove_spikes(repaired);
	::make_valid(repaired);
	geom::correct(repaired);
	remove_collapsed_rings(repaired);

	if (!geom::is_empty(repaired) && geom::is_valid(repaired, failure)) {
		mp = std::move(repaired);
		result.changed = true;
		return result;
	}

	result.valid = false;
	return result;
}

inline bool is_valid_mvt_polygon(const MultiPolygon& mp) {
	if (geom::is_empty(mp)) {
		return false;
	}

	geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
	if (!geom::is_valid(mp, failure)) {
		return false;
	}

	for (const auto& polygon : mp) {
		if (signed_area(polygon.outer()) <= 0.0) {
			return false;
		}
		for (const auto& inner : polygon.inners()) {
			if (signed_area(inner) >= 0.0) {
				return false;
			}
		}
	}
	return true;
}

} // namespace geometry
} // namespace tilemaker

#endif
