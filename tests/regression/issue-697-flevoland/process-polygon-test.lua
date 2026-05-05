function zmin_for_area(min_square_pixels, way_area)
	local circumfence = 40052725.78
	local zmin = (math.log((min_square_pixels * circumfence^2) / (2^16 * way_area))) / (2 * math.log(2))
	return math.floor(zmin)
end

function process_water_polygons(way_area)
	local natural = Find("natural")
	if natural ~= "water" then
		return
	end
	mz = math.max(4, zmin_for_area(0.1, way_area))
	Layer("water_polygons", true)
	MinZoom(mz)
	Attribute("osm_id", Id())
end

function way_function()
	local area = Area()
	if area > 0 then
		process_water_polygons(area)
	end
end

function init_function()
end
function exit_function()
end

node_keys = { }
function node_function()
end
