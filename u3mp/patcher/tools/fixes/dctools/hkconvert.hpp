// dctools/hkconvert.hpp: Havok packfile converter, PS3 (hk_2010.2.0-r1, 32-bit BE) -> PS4 (hk_2011.3.0-r1, 64-bit LE);
// the actor toolset's hkconvert.py in C++.
//
// Reflection-driven: the class layouts come from the hkClass / hkClassMember tables of both games' executables
// (hk2010_ps3.json, hk2011_ps4.json: made from the player's own executables by the actors-hk-tables step, never
// shipped), plus the PS4 classes whose member tables are built at run time (layouts checked against retail
// packfiles). Members are matched by name; members missing on PS3 are zero, not-serializable (0x400) members zero.
// The 2010 -> 2011 model changes: breakable shapes get an hkpSimpleBreakableMaterial, controllers a breaking impulse,
// hkpConvexTransformShape a QS transform. Objects are written in the Havok packfile writer's order (depth first from
// the contents object, pointers in PS4 member order, each object followed by its arrays and strings).
#pragma once
#include "dc.hpp"

namespace u3::hk {

// the tables (the json files of a folder), loaded once
void load_tables(const fs::path &ps3_json, const fs::path &ps4_json);
Bytes convert_packfile(const Bytes &b3);

}  // namespace u3::hk
