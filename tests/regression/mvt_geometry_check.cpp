#include <boost/geometry.hpp>
#include <boost/geometry/geometries/geometries.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <vtzero/geometry.hpp>
#include <vtzero/vector_tile.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

#ifndef SQLITE_TRANSIENT
#define SQLITE_TRANSIENT reinterpret_cast<sqlite3_destructor_type>(-1)
#endif

namespace bg = boost::geometry;

using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using MultiPolygon = bg::model::multi_polygon<Polygon>;

static std::string readFile(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		throw std::runtime_error("could not open tile file: " + path);
	}

	std::ostringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

static bool isGzip(const std::string& data) {
	return data.size() >= 2 &&
		static_cast<unsigned char>(data[0]) == 0x1f &&
		static_cast<unsigned char>(data[1]) == 0x8b;
}

static std::string gunzip(const std::string& data) {
	z_stream stream{};
	stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
	stream.avail_in = static_cast<uInt>(data.size());

	if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
		throw std::runtime_error("inflateInit2 failed");
	}

	std::string output;
	std::vector<char> buffer(64 * 1024);
	int rc = Z_OK;
	while (rc == Z_OK) {
		stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
		stream.avail_out = static_cast<uInt>(buffer.size());
		rc = inflate(&stream, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END) {
			inflateEnd(&stream);
			throw std::runtime_error("inflate failed");
		}
		output.append(buffer.data(), buffer.size() - stream.avail_out);
	}

	inflateEnd(&stream);
	return output;
}

struct PolygonHandler {
	std::vector<vtzero::point> ring;
	MultiPolygon geometry;
	bool sawOuter = false;
	bool invalidOrder = false;
	std::size_t zeroAreaRings = 0;
	std::size_t shortRings = 0;

	void ring_begin(uint32_t count) {
		ring.clear();
		ring.reserve(count);
	}

	void ring_point(vtzero::point point) {
		ring.push_back(point);
	}

	void ring_end(vtzero::ring_type type) {
		if (type == vtzero::ring_type::invalid) {
			zeroAreaRings++;
			return;
		}

		if (ring.size() < 4) {
			shortRings++;
			return;
		}

		if (type == vtzero::ring_type::outer) {
			Polygon polygon;
			for (const auto& point : ring) {
				bg::append(polygon.outer(), Point(point.x, point.y));
			}
			geometry.push_back(std::move(polygon));
			sawOuter = true;
			return;
		}

		if (!sawOuter || geometry.empty()) {
			invalidOrder = true;
			return;
		}

		auto& inners = geometry.back().inners();
		inners.resize(inners.size() + 1);
		for (const auto& point : ring) {
			bg::append(inners.back(), Point(point.x, point.y));
		}
	}
};

static int usage(const char* argv0) {
	std::cerr << "Usage: " << argv0 << " tile.pbf [layer]\n";
	return 2;
}

int main(int argc, char** argv) {
	if (argc < 2 || argc > 3) {
		return usage(argv[0]);
	}

	const std::string tilePath = argv[1];
	const std::string layerFilter = argc == 3 ? argv[2] : "";

	try {
		std::string tileData = readFile(tilePath);
		std::string tileBytes = isGzip(tileData) ? gunzip(tileData) : tileData;
		vtzero::vector_tile tile(tileBytes);

		std::size_t polygonFeatures = 0;
		std::size_t invalidFeatures = 0;
		while (auto layer = tile.next_layer()) {
			const std::string layerName(layer.name().data(), layer.name().size());
			if (!layerFilter.empty() && layerName != layerFilter) {
				continue;
			}

			while (auto feature = layer.next_feature()) {
				if (feature.geometry_type() != vtzero::GeomType::POLYGON) {
					continue;
				}

				polygonFeatures++;
				PolygonHandler handler;
				try {
					vtzero::decode_polygon_geometry(feature.geometry(), handler);
				} catch (const std::exception& e) {
					invalidFeatures++;
					std::cerr << "Invalid polygon command stream in layer " << layerName
						<< ": " << e.what() << "\n";
					continue;
				}

				if (handler.invalidOrder) {
					invalidFeatures++;
					std::cerr << "Polygon begins with an inner ring in layer " << layerName << "\n";
					continue;
				}

				if (handler.zeroAreaRings || handler.shortRings) {
					invalidFeatures++;
					std::cerr << "Polygon has " << handler.zeroAreaRings << " zero-area rings and "
						<< handler.shortRings << " short rings in layer " << layerName << "\n";
					continue;
				}

				std::string reason;
				if (!handler.geometry.empty() && !bg::is_valid(handler.geometry, reason)) {
					invalidFeatures++;
					std::cerr << "Boost.Geometry invalid polygon in layer " << layerName
						<< ": " << reason << "\n";
				}
			}
		}

		if (polygonFeatures == 0) {
			std::cerr << "No polygon features checked";
			if (!layerFilter.empty()) {
				std::cerr << " in layer " << layerFilter;
			}
			std::cerr << "\n";
			return 1;
		}

		if (invalidFeatures > 0) {
			std::cerr << "Checked " << polygonFeatures << " polygon features, invalid="
				<< invalidFeatures << "\n";
			return 1;
		}

		std::cout << "Checked " << polygonFeatures << " polygon features, invalid=0\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "mvt_geometry_check failed: " << e.what() << "\n";
		return 1;
	}
}
