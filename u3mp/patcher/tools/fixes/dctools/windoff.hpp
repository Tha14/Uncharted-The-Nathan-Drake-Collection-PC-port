// dctools/windoff.hpp: a retail PS4 level pak with the g_pivotWind* strength constants of some GEOMETRY_1 materials
// zeroed (see windoff.cpp).
#pragma once
#include "dc.hpp"

namespace u3 {
// the pak at src with the wind strengths of the materials whose name contains one of subs set to 0.0 (no layout change);
// subs empty: the City of Brass reeds and grass shrubs ('oas-reeds', 'grass-shrub')
Bytes windoff_convert(const fs::path &src, std::vector<std::string> subs);
}  // namespace u3
