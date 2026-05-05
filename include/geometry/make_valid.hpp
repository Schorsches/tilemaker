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

inline bool same_point(const Point& a, const Point& b) {
	return a.x() == b.x() && a.y() == b.y();
}

inline bool is_axis_backtrack(const Point& prev, const Point& curr, const Point& next) {
	if (prev.x() == curr.x() && curr.x() == next.x()) {
		return (prev.y() < curr.y() && next.y() < curr.y()) ||
			(prev.y() > curr.y() && next.y() > curr.y());
	}
	if (prev.y() == curr.y() && curr.y() == next.y()) {
		return (prev.x() < curr.x() && next.x() < curr.x()) ||
			(prev.x() > curr.x() && next.x() > curr.x());
	}
	return false;
}

inline bool sanitize_ring(Ring& ring) {
	if (ring.empty()) {
		return false;
	}

	bool changed = false;
	std::vector<Point> points;
	points.reserve(ring.size());
	for (const Point& point : ring) {
		if (points.empty() || !same_point(points.back(), point)) {
			points.push_back(point);
		} else {
			changed = true;
		}
	}

	if (points.size() > 1 && same_point(points.front(), points.back())) {
		points.pop_back();
	}

	bool localChanged = true;
	while (localChanged && points.size() > 3) {
		localChanged = false;
		for (std::size_t i = 0; i < points.size(); ) {
			const Point& prev = points[(i + points.size() - 1) % points.size()];
			const Point& curr = points[i];
			const Point& next = points[(i + 1) % points.size()];
			const double cross =
				(curr.x() - prev.x()) * (next.y() - curr.y()) -
				(curr.y() - prev.y()) * (next.x() - curr.x());
			if (cross == 0.0 || is_axis_backtrack(prev, curr, next)) {
				points.erase(points.begin() + i);
				localChanged = true;
				changed = true;
				continue;
			}
			++i;
		}
	}

	Ring sanitized;
	sanitized.reserve(points.size() + 1);
	for (const Point& point : points) {
		sanitized.push_back(point);
	}
	if (!sanitized.empty()) {
		sanitized.push_back(sanitized.front());
	}
	if (!changed && sanitized.size() == ring.size()) {
		return false;
	}
	ring = std::move(sanitized);

	return true;
}

inline std::size_t sanitize_mvt_rings(MultiPolygon& mp) {
	std::size_t changed = 0;
	for (auto& polygon : mp) {
		if (sanitize_ring(polygon.outer())) {
			changed++;
		}
		for (auto& inner : polygon.inners()) {
			if (sanitize_ring(inner)) {
				changed++;
			}
		}
	}
	return changed;
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

	MultiPolygon repaired = mp;
	geom::remove_spikes(repaired);
	remove_collapsed_rings(repaired);

	geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
	if (!geom::is_empty(repaired) && geom::is_valid(repaired, failure)) {
		if (!geom::equals(mp, repaired)) {
			mp = std::move(repaired);
			result.changed = true;
		}
		return result;
	}

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

inline bool is_valid_ogc_after_mvt(const MultiPolygon& mp) {
	MultiPolygon ogc = mp;
	geom::correct(ogc);

	std::string reason;
	return !geom::is_empty(ogc) && geom::is_valid(ogc, reason);
}

inline MakeValidResult repair_ogc_then_mvt(MultiPolygon& mp) {
	MakeValidResult result;
	MultiPolygon repaired = mp;
	geom::correct(repaired);
	geom::remove_spikes(repaired);
	::make_valid(repaired);
	geom::correct(repaired);
	remove_collapsed_rings(repaired);

	std::string reason;
	if (geom::is_empty(repaired) || !geom::is_valid(repaired, reason)) {
		result.valid = false;
		return result;
	}

	mp = std::move(repaired);
	enforce_mvt_winding(mp);
	result.changed = true;
	return result;
}

} // namespace geometry
} // namespace tilemaker

#endif
