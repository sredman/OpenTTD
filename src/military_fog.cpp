/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file military_fog.cpp Implementation of fog of war and knowledge-limited military scouting. */

#include "stdafx.h"
#include "military_fog.h"
#include "disaster_vehicle.h"
#include "company_func.h"
#include "town_map.h"
#include "saveload/saveload.h"
#include <algorithm>
#include "safeguards.h"

/** Whether military fog of war is globally active. */
static bool _military_fog_active = false;
/** Mapping from company ID to intelligence side. */
static std::array<uint8_t, MAX_COMPANIES> _company_intel_side{};
/** Intelligence state for each military side. */
static std::array<SideIntelligence, MAX_MILITARY_SIDES> _side_intelligence{};


void SideIntelligence::Allocate(size_t map_size)
{
	this->is_scouted.assign(map_size, 0);
	this->currently_visible.assign(map_size, 0);
	this->base_tiles = std::make_unique<Tile::TileBase[]>(map_size);
	this->extended_tiles = std::make_unique<Tile::TileExtended[]>(map_size);
	this->town_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = static_cast<uint32_t>(map_size);
}

void SideIntelligence::Reset()
{
	this->is_scouted.clear();
	this->currently_visible.clear();
	this->base_tiles.reset();
	this->extended_tiles.reset();
	this->town_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = 0;
}

bool MilitaryFog::IsActive()
{
	return _military_fog_active;
}

void MilitaryFog::SetActive(bool active)
{
	_military_fog_active = active;
}

void MilitaryFog::Initialize()
{
	_military_fog_active = true;

	/* Default intelligence affiliation: company % 2 */
	for (size_t c = 0; c < MAX_COMPANIES; c++) {
		_company_intel_side[c] = static_cast<uint8_t>(c % MAX_MILITARY_SIDES);
	}

	size_t map_size = Map::Size();
	if (map_size == 0) return;

	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		_side_intelligence[s].Allocate(map_size);

		/* Initial stationary snapshot matches starting world geography */
		if (Tile::base_tiles != nullptr) {
			std::copy_n(Tile::base_tiles.get(), map_size, _side_intelligence[s].base_tiles.get());
		}
		if (Tile::extended_tiles != nullptr) {
			std::copy_n(Tile::extended_tiles.get(), map_size, _side_intelligence[s].extended_tiles.get());
		}

		/* Snapshot starting towns */
		_side_intelligence[s].town_snapshots.resize(Town::GetPoolSize());
		for (const Town *t : Town::Iterate()) {
			if (_side_intelligence[s].town_snapshots.size() <= t->index.base()) {
				_side_intelligence[s].town_snapshots.resize(t->index.base() + 64);
			}
			TownSnapshot &ts = _side_intelligence[s].town_snapshots[t->index.base()];
			ts.population = t->cache.population;
			ts.num_houses = t->cache.num_houses;
			ts.name = t->name;
			ts.exists = true;
		}

		/* Snapshot starting industries */
		_side_intelligence[s].industry_snapshots.resize(Industry::GetPoolSize());
		for (const Industry *i : Industry::Iterate()) {
			if (_side_intelligence[s].industry_snapshots.size() <= i->index.base()) {
				_side_intelligence[s].industry_snapshots.resize(i->index.base() + 64);
			}
			IndustrySnapshot &is = _side_intelligence[s].industry_snapshots[i->index.base()];
			is.type = i->type;
			is.location = i->location.tile;
			is.prod_level = i->prod_level;
			is.exists = true;
		}

		/* All tiles begin under fog of war as never-scouted */
		_side_intelligence[s].never_scouted_count = static_cast<uint32_t>(map_size);
	}

	/* Update current visibility according to active military aircraft */
	UpdateAllVisibility();
}

void MilitaryFog::Reset()
{
	_military_fog_active = false;
	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		_side_intelligence[s].Reset();
	}
}

uint8_t MilitaryFog::GetCompanyIntelligenceSide(CompanyID company)
{
	if (!Company::IsValidID(company)) return 0;
	return _company_intel_side[company.base()] % MAX_MILITARY_SIDES;
}

void MilitaryFog::SetCompanyIntelligenceSide(CompanyID company, uint8_t side)
{
	if (Company::IsValidID(company)) {
		_company_intel_side[company.base()] = side % MAX_MILITARY_SIDES;
	}
}

uint8_t MilitaryFog::GetLocalIntelligenceSide()
{
	if (Company::IsValidID(_local_company)) {
		return GetCompanyIntelligenceSide(_local_company);
	}
	return 0;
}

FogStatus MilitaryFog::GetTileFogStatus(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active) return FOG_CURRENTLY_VISIBLE;
	if (side >= MAX_MILITARY_SIDES || !IsValidTile(tile)) return FOG_NEVER_SCOUTED;
	const auto &intel = _side_intelligence[side];
	if (tile.base() >= intel.currently_visible.size() || tile.base() >= intel.is_scouted.size()) return FOG_NEVER_SCOUTED;

	if (intel.currently_visible[tile.base()]) return FOG_CURRENTLY_VISIBLE;
	if (intel.is_scouted[tile.base()]) return FOG_PREVIOUSLY_SCOUTED;
	return FOG_NEVER_SCOUTED;
}

bool MilitaryFog::IsTileCurrentlyVisible(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active) return true;
	if (side >= MAX_MILITARY_SIDES || !IsValidTile(tile)) return false;
	const auto &intel = _side_intelligence[side];
	if (tile.base() >= intel.currently_visible.size()) return false;
	return intel.currently_visible[tile.base()] != 0;
}

bool MilitaryFog::IsTileScouted(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active) return false;
	if (side >= MAX_MILITARY_SIDES || !IsValidTile(tile)) return false;
	const auto &intel = _side_intelligence[side];
	if (tile.base() >= intel.is_scouted.size()) return false;
	return intel.is_scouted[tile.base()] != 0;
}

FogStatus MilitaryFog::GetTileFogStatusForLocal(TileIndex tile)
{
	return GetTileFogStatus(GetLocalIntelligenceSide(), tile);
}

bool MilitaryFog::IsTileCurrentlyVisibleForLocal(TileIndex tile)
{
	return IsTileCurrentlyVisible(GetLocalIntelligenceSide(), tile);
}

bool MilitaryFog::ShouldHideVehicle(uint8_t side, const Vehicle *v)
{
	if (!_military_fog_active || v == nullptr) return false;
	if (v->type == VehicleType::Effect) return false;

	/* Player's own transport company vehicles are never hidden from the local player */
	if (v->owner != OWNER_NONE && Company::IsValidID(_local_company) && v->owner == _local_company) {
		return false;
	}

	/* Military units */
	if (v->type == VehicleType::Disaster) {
		const DisasterVehicle *dv = static_cast<const DisasterVehicle *>(v);
		if (dv->subtype == ST_MILITARY_AIRCRAFT ||
				dv->subtype == ST_MILITARY_AIRCRAFT_SHADOW ||
				dv->subtype == ST_MILITARY_TARGET_MARKER) {
			if (dv->GetMilitarySide() == side) return false; // Friendly military unit
			return !IsTileCurrentlyVisible(side, v->tile);
		}
		return false;
	}

	/* Competitor transport company vehicles disappear when behind fog, even if affiliated with the same side */
	if (v->owner != OWNER_NONE) {
		return !IsTileCurrentlyVisible(side, v->tile);
	}

	return false;
}


bool MilitaryFog::ShouldHideVehicleForLocal(const Vehicle *v)
{
	return ShouldHideVehicle(GetLocalIntelligenceSide(), v);
}

void MilitaryFog::RefreshStationarySnapshot(uint8_t side, TileIndex tile)
{
	if (side >= MAX_MILITARY_SIDES || !IsValidTile(tile)) return;
	SideIntelligence &intel = _side_intelligence[side];
	if (intel.base_tiles == nullptr || intel.extended_tiles == nullptr) return;

	/* Refresh raw tile data */
	intel.base_tiles[tile.base()] = Tile::base_tiles[tile.base()];
	intel.extended_tiles[tile.base()] = Tile::extended_tiles[tile.base()];

	/* Refresh town snapshot if tile belongs to a town */
	if (IsTileType(tile, TileType::House)) {
		TownID tid = GetTownIndex(tile);
		if (Town::IsValidID(tid)) {
			const Town *t = Town::Get(tid);
			if (intel.town_snapshots.size() <= tid.base()) {
				intel.town_snapshots.resize(tid.base() + 64);
			}
			TownSnapshot &ts = intel.town_snapshots[tid.base()];
			ts.population = t->cache.population;
			ts.num_houses = t->cache.num_houses;
			ts.name = t->name;
			ts.exists = true;
		}
	}

	/* Refresh industry snapshot if tile belongs to an industry */
	if (IsTileType(tile, TileType::Industry)) {
		IndustryID iid = GetIndustryIndex(tile);
		if (Industry::IsValidID(iid)) {
			const Industry *i = Industry::Get(iid);
			if (intel.industry_snapshots.size() <= iid.base()) {
				intel.industry_snapshots.resize(iid.base() + 64);
			}
			IndustrySnapshot &is = intel.industry_snapshots[iid.base()];
			is.type = i->type;
			is.location = i->location.tile;
			is.prod_level = i->prod_level;
			is.exists = true;
		}
	}
}

void MilitaryFog::UpdateAircraftScouting(DisasterVehicle *aircraft)
{
	if (!_military_fog_active || aircraft == nullptr) return;
	uint8_t side = aircraft->GetMilitarySide();
	if (side >= MAX_MILITARY_SIDES) return;

	SideIntelligence &intel = _side_intelligence[side];
	if (intel.is_scouted.empty()) return;

	TileIndex center_tile = TileVirtXY(aircraft->x_pos, aircraft->y_pos);
	int cx = TileX(center_tile);
	int cy = TileY(center_tile);
	int r = static_cast<int>(MILITARY_SCOUT_RADIUS);

	for (int dy = -r; dy <= r; dy++) {
		for (int dx = -r; dx <= r; dx++) {
			int tx = cx + dx;
			int ty = cy + dy;
			if (tx < 0 || tx >= static_cast<int>(Map::SizeX()) || ty < 0 || ty >= static_cast<int>(Map::SizeY())) continue;
			if (dx * dx + dy * dy > r * r) continue;

			TileIndex t = TileXY(tx, ty);
			intel.currently_visible[t.base()] = 1;
			if (intel.is_scouted[t.base()] == 0) {
				intel.is_scouted[t.base()] = 1;
				if (intel.never_scouted_count > 0) intel.never_scouted_count--;
			}
			RefreshStationarySnapshot(side, t);
		}
	}

	/* Also explicitly refresh town and industry snapshots if their center/location is within radius */
	for (const Town *t : Town::Iterate()) {
		if (DistanceSquare(t->xy, center_tile) <= static_cast<uint>(r * r)) {
			if (intel.town_snapshots.size() <= t->index.base()) {
				intel.town_snapshots.resize(t->index.base() + 64);
			}
			TownSnapshot &ts = intel.town_snapshots[t->index.base()];
			ts.population = t->cache.population;
			ts.num_houses = t->cache.num_houses;
			ts.name = t->name;
			ts.exists = true;
		}
	}

	for (const Industry *i : Industry::Iterate()) {
		if (DistanceSquare(i->location.tile, center_tile) <= static_cast<uint>(r * r)) {
			if (intel.industry_snapshots.size() <= i->index.base()) {
				intel.industry_snapshots.resize(i->index.base() + 64);
			}
			IndustrySnapshot &is = intel.industry_snapshots[i->index.base()];
			is.type = i->type;
			is.location = i->location.tile;
			is.prod_level = i->prod_level;
			is.exists = true;
		}
	}

	/* Discover enemy contacts in scouting radius */
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype != ST_MILITARY_TARGET_MARKER && v->subtype != ST_MILITARY_AIRCRAFT) continue;
		if (v->GetMilitarySide() == side) continue; // Friendly unit
		if (v->vehstatus.Test(VehState::Crashed)) continue;

		int64_t edx = (int64_t)aircraft->x_pos - v->x_pos;
		int64_t edy = (int64_t)aircraft->y_pos - v->y_pos;
		uint64_t dist = edx * edx + edy * edy;
		uint64_t max_pixel_dist = (uint64_t)(r * TILE_SIZE) * (uint64_t)(r * TILE_SIZE);

		if (dist <= max_pixel_dist) {
			/* Enemy discovered */
			bool found = false;
			for (auto &c : intel.contacts) {
				if (c.vehicle_id == v->index) {
					c.last_x = v->x_pos;
					c.last_y = v->y_pos;
					c.last_tile = v->tile;
					c.currently_visible = true;
					found = true;
					break;
				}
			}
			if (!found) {
				MilitaryContact c;
				c.vehicle_id = v->index;
				c.last_x = v->x_pos;
				c.last_y = v->y_pos;
				c.last_tile = v->tile;
				c.side = v->GetMilitarySide();
				c.currently_visible = true;
				intel.contacts.push_back(c);
			}
		}
	}
}

void MilitaryFog::UpdateAllVisibility()
{
	if (!_military_fog_active) return;

	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		std::fill(_side_intelligence[s].currently_visible.begin(), _side_intelligence[s].currently_visible.end(), 0);
	}

	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT && !v->vehstatus.Test(VehState::Crashed)) {
			UpdateAircraftScouting(v);
		}
	}

	/* Update currently_visible flags for retained contacts */
	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		for (auto &c : _side_intelligence[s].contacts) {
			if (!IsTileCurrentlyVisible(s, c.last_tile)) {
				c.currently_visible = false;
			}
		}
	}
}

const MilitaryContact *MilitaryFog::FindNearestEligibleEnemyContact(const DisasterVehicle *aircraft)
{
	if (!_military_fog_active || aircraft == nullptr) return nullptr;
	uint8_t side = aircraft->GetMilitarySide();
	if (side >= MAX_MILITARY_SIDES) return nullptr;

	const auto &contacts = _side_intelligence[side].contacts;
	if (contacts.empty()) return nullptr;

	const MilitaryContact *best = nullptr;
	uint64_t best_dist = UINT64_MAX;

	for (const auto &c : contacts) {
		int64_t dx = (int64_t)aircraft->x_pos - c.last_x;
		int64_t dy = (int64_t)aircraft->y_pos - c.last_y;
		uint64_t dist = dx * dx + dy * dy;

		if (dist < best_dist) {
			best_dist = dist;
			best = &c;
		} else if (dist == best_dist && best != nullptr) {
			/* Deterministic tie-breaking by lowest contact VehicleID */
			if (c.vehicle_id < best->vehicle_id) {
				best = &c;
			}
		}
	}

	return best;
}

TileIndex MilitaryFog::FindNearestNeverScoutedTile(const DisasterVehicle *aircraft)
{
	if (!_military_fog_active || aircraft == nullptr) return INVALID_TILE;
	uint8_t side = aircraft->GetMilitarySide();
	if (side >= MAX_MILITARY_SIDES) return INVALID_TILE;

	const auto &intel = _side_intelligence[side];
	if (intel.never_scouted_count == 0 || intel.is_scouted.empty()) return INVALID_TILE;

	TileIndex best_tile = INVALID_TILE;
	uint64_t best_dist = UINT64_MAX;

	size_t size = Map::Size();
	for (size_t t = 0; t < size; t++) {
		if (intel.is_scouted[t] != 0) continue;

		TileIndex tile{static_cast<uint>(t)};
		int64_t center_x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
		int64_t center_y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
		int64_t dx = (int64_t)aircraft->x_pos - center_x;
		int64_t dy = (int64_t)aircraft->y_pos - center_y;
		uint64_t dist = dx * dx + dy * dy;

		if (dist < best_dist) {
			best_dist = dist;
			best_tile = tile;
		} else if (dist == best_dist && best_tile != INVALID_TILE) {
			/* Deterministic tie-breaking by lowest TileIndex */
			if (tile < best_tile) {
				best_tile = tile;
			}
		}
	}

	return best_tile;
}

bool MilitaryFog::CheckArrivalAndClearEmptyContact(DisasterVehicle *aircraft)
{
	if (!_military_fog_active || aircraft == nullptr) return false;
	if (aircraft->big_ufo_destroyer_target == VehicleID::Invalid()) return false;

	uint8_t side = aircraft->GetMilitarySide();
	if (side >= MAX_MILITARY_SIDES) return false;

	auto &contacts = _side_intelligence[side].contacts;
	for (auto it = contacts.begin(); it != contacts.end(); ++it) {
		if (it->vehicle_id == aircraft->big_ufo_destroyer_target) {
			/* Check if target is actually present at arrival position */
			DisasterVehicle *target = DisasterVehicle::GetIfValid(it->vehicle_id);
			bool present = false;
			if (target != nullptr &&
					(target->subtype == ST_MILITARY_TARGET_MARKER || target->subtype == ST_MILITARY_AIRCRAFT) &&
					target->GetMilitarySide() != side &&
					!target->vehstatus.Test(VehState::Crashed)) {
				int dist_x = std::abs(target->x_pos - aircraft->x_pos);
				int dist_y = std::abs(target->y_pos - aircraft->y_pos);
				if (dist_x + dist_y <= 16) {
					present = true;
				}
			}

			if (!present) {
				/* Location revisited empty: clear retained contact */
				contacts.erase(it);
				aircraft->big_ufo_destroyer_target = VehicleID::Invalid();
				return true;
			}
			break;
		}
	}

	return false;
}

SideIntelligence &MilitaryFog::GetSideIntelligence(uint8_t side)
{
	assert(side < MAX_MILITARY_SIDES);
	return _side_intelligence[side];
}

const TownSnapshot *MilitaryFog::GetTownSnapshot(uint8_t side, TownID town)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	const auto &snaps = _side_intelligence[side].town_snapshots;
	if (town.base() >= snaps.size() || !snaps[town.base()].exists) return nullptr;
	return &snaps[town.base()];
}

const IndustrySnapshot *MilitaryFog::GetIndustrySnapshot(uint8_t side, IndustryID industry)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	const auto &snaps = _side_intelligence[side].industry_snapshots;
	if (industry.base() >= snaps.size() || !snaps[industry.base()].exists) return nullptr;
	return &snaps[industry.base()];
}

void MilitaryFog::ReconstructVisibility(bool refresh_unseen)
{
	if (!_military_fog_active) return;

	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		std::fill(_side_intelligence[s].currently_visible.begin(), _side_intelligence[s].currently_visible.end(), 0);
	}

	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT && !v->vehstatus.Test(VehState::Crashed)) {
			UpdateAircraftScouting(v);
		}
	}

	if (refresh_unseen) {
		for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
			for (size_t t = 0; t < Map::Size(); t++) {
				if (_side_intelligence[s].is_scouted[t]) {
					RefreshStationarySnapshot(s, TileIndex{static_cast<uint>(t)});
				}
			}
		}
	}
}

ActiveSnapshotScope::ActiveSnapshotScope(uint8_t side)
{
	if (!MilitaryFog::IsActive() || side >= MAX_MILITARY_SIDES) return;
	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side);
	if (intel.base_tiles == nullptr || intel.extended_tiles == nullptr) return;

	this->prev_base = Tile::active_base_tiles;
	this->prev_extended = Tile::active_extended_tiles;
	Tile::active_base_tiles = intel.base_tiles.get();
	Tile::active_extended_tiles = intel.extended_tiles.get();
	this->active = true;
}

ActiveSnapshotScope::~ActiveSnapshotScope()
{
	if (this->active) {
		Tile::active_base_tiles = this->prev_base;
		Tile::active_extended_tiles = this->prev_extended;
	}
}

/* Save / load implementation for MFOG chunk */
void MilitaryFog::Save()
{
	SlWriteByte(_military_fog_active ? 1 : 0);
	if (!_military_fog_active) return;

	/* Save company intelligence side mappings */
	for (size_t c = 0; c < MAX_COMPANIES; c++) {
		SlWriteByte(_company_intel_side[c]);
	}

	size_t map_size = Map::Size();
	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		SideIntelligence &intel = _side_intelligence[s];

		/* Save exploration is_scouted */
		for (size_t i = 0; i < map_size; i++) {
			SlWriteByte(intel.is_scouted[i]);
		}

		/* Save base_tiles */
		for (size_t i = 0; i < map_size; i++) {
			SlWriteByte(intel.base_tiles[i].type);
			SlWriteByte(intel.base_tiles[i].height);
			SlWriteByte(GB(intel.base_tiles[i].m2, 0, 8));
			SlWriteByte(GB(intel.base_tiles[i].m2, 8, 8));
			SlWriteByte(intel.base_tiles[i].m1);
			SlWriteByte(intel.base_tiles[i].m3);
			SlWriteByte(intel.base_tiles[i].m4);
			SlWriteByte(intel.base_tiles[i].m5);
		}

		/* Save extended_tiles */
		for (size_t i = 0; i < map_size; i++) {
			SlWriteByte(intel.extended_tiles[i].m6);
			SlWriteByte(intel.extended_tiles[i].m7);
			SlWriteByte(GB(intel.extended_tiles[i].m8, 0, 8));
			SlWriteByte(GB(intel.extended_tiles[i].m8, 8, 8));
		}

		/* Save contacts */
		uint32_t num_contacts = static_cast<uint32_t>(intel.contacts.size());
		SlWriteByte(GB(num_contacts, 24, 8));
		SlWriteByte(GB(num_contacts, 16, 8));
		SlWriteByte(GB(num_contacts, 8, 8));
		SlWriteByte(GB(num_contacts, 0, 8));
		for (const auto &c : intel.contacts) {
			/* VehicleID (32-bit) */
			SlWriteByte(GB(c.vehicle_id.base(), 24, 8));
			SlWriteByte(GB(c.vehicle_id.base(), 16, 8));
			SlWriteByte(GB(c.vehicle_id.base(), 8, 8));
			SlWriteByte(GB(c.vehicle_id.base(), 0, 8));
			/* Pixel coordinates last_x, last_y (32-bit signed) */
			uint32_t ux = static_cast<uint32_t>(c.last_x);
			SlWriteByte(GB(ux, 24, 8));
			SlWriteByte(GB(ux, 16, 8));
			SlWriteByte(GB(ux, 8, 8));
			SlWriteByte(GB(ux, 0, 8));
			uint32_t uy = static_cast<uint32_t>(c.last_y);
			SlWriteByte(GB(uy, 24, 8));
			SlWriteByte(GB(uy, 16, 8));
			SlWriteByte(GB(uy, 8, 8));
			SlWriteByte(GB(uy, 0, 8));
			/* TileIndex (32-bit) */
			SlWriteByte(GB(c.last_tile.base(), 24, 8));
			SlWriteByte(GB(c.last_tile.base(), 16, 8));
			SlWriteByte(GB(c.last_tile.base(), 8, 8));
			SlWriteByte(GB(c.last_tile.base(), 0, 8));
			/* Side and visibility */
			SlWriteByte(c.side);
			SlWriteByte(c.currently_visible ? 1 : 0);
		}
	}
}

void MilitaryFog::Load()
{
	uint8_t active = SlReadByte();
	_military_fog_active = (active != 0);
	if (!_military_fog_active) {
		Reset();
		return;
	}

	for (size_t c = 0; c < MAX_COMPANIES; c++) {
		_company_intel_side[c] = SlReadByte();
	}

	size_t map_size = Map::Size();
	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		SideIntelligence &intel = _side_intelligence[s];
		intel.Allocate(map_size);

		intel.never_scouted_count = 0;
		for (size_t i = 0; i < map_size; i++) {
			intel.is_scouted[i] = SlReadByte();
			if (intel.is_scouted[i] == 0) intel.never_scouted_count++;
		}

		for (size_t i = 0; i < map_size; i++) {
			intel.base_tiles[i].type = SlReadByte();
			intel.base_tiles[i].height = SlReadByte();
			uint8_t m2_lo = SlReadByte();
			uint8_t m2_hi = SlReadByte();
			intel.base_tiles[i].m2 = (uint16_t)(m2_lo | (m2_hi << 8));
			intel.base_tiles[i].m1 = SlReadByte();
			intel.base_tiles[i].m3 = SlReadByte();
			intel.base_tiles[i].m4 = SlReadByte();
			intel.base_tiles[i].m5 = SlReadByte();
		}

		for (size_t i = 0; i < map_size; i++) {
			intel.extended_tiles[i].m6 = SlReadByte();
			intel.extended_tiles[i].m7 = SlReadByte();
			uint8_t m8_lo = SlReadByte();
			uint8_t m8_hi = SlReadByte();
			intel.extended_tiles[i].m8 = (uint16_t)(m8_lo | (m8_hi << 8));
		}

		uint32_t num_contacts = (static_cast<uint32_t>(SlReadByte()) << 24);
		num_contacts |= (static_cast<uint32_t>(SlReadByte()) << 16);
		num_contacts |= (static_cast<uint32_t>(SlReadByte()) << 8);
		num_contacts |= static_cast<uint32_t>(SlReadByte());
		intel.contacts.clear();
		for (uint32_t j = 0; j < num_contacts; j++) {
			MilitaryContact c;
			uint32_t vid = (static_cast<uint32_t>(SlReadByte()) << 24);
			vid |= (static_cast<uint32_t>(SlReadByte()) << 16);
			vid |= (static_cast<uint32_t>(SlReadByte()) << 8);
			vid |= static_cast<uint32_t>(SlReadByte());
			c.vehicle_id = VehicleID{vid};

			uint32_t lx = (static_cast<uint32_t>(SlReadByte()) << 24);
			lx |= (static_cast<uint32_t>(SlReadByte()) << 16);
			lx |= (static_cast<uint32_t>(SlReadByte()) << 8);
			lx |= static_cast<uint32_t>(SlReadByte());
			c.last_x = static_cast<int>(lx);

			uint32_t ly = (static_cast<uint32_t>(SlReadByte()) << 24);
			ly |= (static_cast<uint32_t>(SlReadByte()) << 16);
			ly |= (static_cast<uint32_t>(SlReadByte()) << 8);
			ly |= static_cast<uint32_t>(SlReadByte());
			c.last_y = static_cast<int>(ly);

			uint32_t lt = (static_cast<uint32_t>(SlReadByte()) << 24);
			lt |= (static_cast<uint32_t>(SlReadByte()) << 16);
			lt |= (static_cast<uint32_t>(SlReadByte()) << 8);
			lt |= static_cast<uint32_t>(SlReadByte());
			c.last_tile = TileIndex{lt};

			c.side = SlReadByte();
			c.currently_visible = (SlReadByte() != 0);
			intel.contacts.push_back(c);
		}
	}

	ReconstructVisibility(false);
}

/** Chunk handler for MFOG (military fog of war) savegame data. */
struct MFOGChunkHandler : ChunkHandler {
	/** Construct the MFOG chunk handler. */
	MFOGChunkHandler() : ChunkHandler("MFOG", ChunkType::Riff) {}

	void Save() const override
	{
		size_t len = 1;
		if (MilitaryFog::IsActive()) {
			len += MAX_COMPANIES;
			size_t map_size = Map::Size();
			for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
				len += map_size; // is_scouted
				len += map_size * 8; // base_tiles
				len += map_size * 4; // extended_tiles
				len += 4 + MilitaryFog::GetSideIntelligence(s).contacts.size() * 18;
			}
		}
		SlSetLength(len);
		MilitaryFog::Save();
	}

	void Load() const override
	{
		MilitaryFog::Load();
	}

};

/** Instance of chunk handler for MFOG savegame data. */
static const MFOGChunkHandler MFOG;
/** Table of chunk handlers for military fog subsystem. */
static const ChunkHandlerRef military_fog_chunk_handlers[] = { MFOG };
/**
 * Exported chunk handler table reference for military fog subsystem.
 * @return ChunkHandlerTable containing military fog chunk handlers.
 */
extern const ChunkHandlerTable _military_fog_chunk_handlers(military_fog_chunk_handlers);
