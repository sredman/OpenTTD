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
#include "viewport_func.h"
#include "vehicle_func.h"
#include "language.h"
#include <algorithm>
#include "safeguards.h"

/** Whether military fog of war is globally active. */
static bool _military_fog_active = false;
/** Mapping from company ID to intelligence side. */
static std::array<uint8_t, MAX_COMPANIES> _company_intel_side{};
/** Intelligence state for each military side. */
static std::array<SideIntelligence, MAX_MILITARY_SIDES> _side_intelligence{};

/**
 * Get safe cached name of an industry without triggering unloaded string table assertion in headless/test environments.
 * @param i The industry to query.
 * @return The cached industry name, or empty string.
 */
static std::string GetSafeIndustryName(const Industry *i)
{
	if (i == nullptr) return "";
	if (!i->cached_name.empty()) return i->cached_name;
	if (_current_language != nullptr && i->town != nullptr) {
		return i->GetCachedName();
	}
	return "";
}


void SideIntelligence::Allocate(size_t map_size)
{
	this->is_scouted.assign(map_size, 0);
	this->currently_visible.assign(map_size, 0);
	this->tile_to_industry_handle.assign(map_size, 0);
	this->base_tiles = std::make_unique<Tile::TileBase[]>(map_size);
	this->extended_tiles = std::make_unique<Tile::TileExtended[]>(map_size);
	this->town_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = static_cast<uint32_t>(map_size);
	this->next_industry_handle = 1;
}

void SideIntelligence::Reset()
{
	this->is_scouted.clear();
	this->currently_visible.clear();
	this->tile_to_industry_handle.clear();
	this->base_tiles.reset();
	this->extended_tiles.reset();
	this->town_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = 0;
	this->next_industry_handle = 1;
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
			ts.xy = t->xy;
			ts.larger_town = t->larger_town;
			ts.exists = true;
		}

		/* Snapshot starting industries with location-anchored handles and footprint tiles */
		_side_intelligence[s].industry_snapshots.clear();
		for (const Industry *i : Industry::Iterate()) {
			uint32_t handle = _side_intelligence[s].next_industry_handle++;
			IndustrySnapshot is;
			is.handle = handle;
			is.type = i->type;
			is.location = i->location.tile;
			is.prod_level = i->prod_level;
			is.owner = i->owner;
			is.colour = i->random_colour;
			is.name = GetSafeIndustryName(i);
			is.exists = true;

			for (TileIndex cur_tile : i->location) {
				if (IsTileType(cur_tile, TileType::Industry) && GetIndustryIndex(cur_tile) == i->index) {
					is.observed_tiles.push_back(cur_tile);
					_side_intelligence[s].tile_to_industry_handle[cur_tile.base()] = handle;
				}
			}
			if (is.observed_tiles.empty() && IsValidTile(i->location.tile)) {
				is.observed_tiles.push_back(i->location.tile);
				_side_intelligence[s].tile_to_industry_handle[i->location.tile.base()] = handle;
			}
			_side_intelligence[s].industry_snapshots.push_back(std::move(is));
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
			ts.xy = t->xy;
			ts.larger_town = t->larger_town;
			ts.exists = true;
		}
	}

	/* Refresh industry snapshot if tile belongs to an industry */
	if (IsTileType(tile, TileType::Industry)) {
		const Industry *i = Industry::GetByTileIfValid(tile);
		if (i != nullptr) {
			TileIndex root = i->location.tile;
			uint32_t prev_handle = (tile.base() < intel.tile_to_industry_handle.size()) ? intel.tile_to_industry_handle[tile.base()] : 0;
			IndustrySnapshot *prev_snap = (prev_handle != 0) ? const_cast<IndustrySnapshot *>(GetIndustrySnapshotByHandle(side, prev_handle)) : nullptr;

			/* If previous snapshot matches current live industry attributes, keep it */
			if (prev_snap != nullptr && prev_snap->location == root && prev_snap->type == i->type &&
					prev_snap->colour == i->random_colour && prev_snap->name == GetSafeIndustryName(i)) {
				if (tile == root) {
					prev_snap->prod_level = i->prod_level;
					prev_snap->owner = i->owner;
				}
				if (std::find(prev_snap->observed_tiles.begin(), prev_snap->observed_tiles.end(), tile) == prev_snap->observed_tiles.end()) {
					prev_snap->observed_tiles.push_back(tile);
				}
				intel.tile_to_industry_handle[tile.base()] = prev_snap->handle;
			} else {
				/* Tile changed or is new: detach tile from previous snapshot without mutating remaining hidden tiles */
				if (prev_snap != nullptr) {
					auto it = std::find(prev_snap->observed_tiles.begin(), prev_snap->observed_tiles.end(), tile);
					if (it != prev_snap->observed_tiles.end()) {
						prev_snap->observed_tiles.erase(it);
					}
					if (prev_snap->observed_tiles.empty()) {
						prev_snap->exists = false;
					}
				}

				/* Find if an existing active snapshot already represents this new live industry appearance at root */
				IndustrySnapshot *snap = nullptr;
				for (auto &existing : intel.industry_snapshots) {
					if (existing.exists && existing.location == root && existing.type == i->type &&
							existing.colour == i->random_colour && existing.name == GetSafeIndustryName(i)) {
						snap = &existing;
						break;
					}
				}
				if (snap == nullptr) {
					uint32_t handle = intel.next_industry_handle++;
					intel.industry_snapshots.emplace_back();
					snap = &intel.industry_snapshots.back();
					snap->handle = handle;
					snap->location = root;
					snap->type = i->type;
					snap->colour = i->random_colour;
					snap->name = GetSafeIndustryName(i);
					snap->owner = (prev_snap != nullptr) ? prev_snap->owner : i->owner;
					snap->prod_level = (prev_snap != nullptr) ? prev_snap->prod_level : 0;
				}
				if (tile == root) {
					snap->prod_level = i->prod_level;
					snap->owner = i->owner;
				}
				snap->exists = true;

				if (std::find(snap->observed_tiles.begin(), snap->observed_tiles.end(), tile) == snap->observed_tiles.end()) {
					snap->observed_tiles.push_back(tile);
				}
				intel.tile_to_industry_handle[tile.base()] = snap->handle;
			}
		}
	} else {
		/* Tile is no longer an industry */
		uint32_t prev_handle = (tile.base() < intel.tile_to_industry_handle.size()) ? intel.tile_to_industry_handle[tile.base()] : 0;
		if (prev_handle != 0) {
			intel.tile_to_industry_handle[tile.base()] = 0;
			for (auto &existing : intel.industry_snapshots) {
				if (existing.handle == prev_handle) {
					auto it = std::find(existing.observed_tiles.begin(), existing.observed_tiles.end(), tile);
					if (it != existing.observed_tiles.end()) {
						existing.observed_tiles.erase(it);
					}
					if (existing.observed_tiles.empty()) {
						existing.exists = false;
					}
					break;
				}
			}
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

	/* Also explicitly refresh town snapshots if their center/location is within radius */
	for (const Town *t : Town::Iterate()) {
		if (DistanceSquare(t->xy, center_tile) <= static_cast<uint>(r * r)) {
			if (intel.town_snapshots.size() <= t->index.base()) {
				intel.town_snapshots.resize(t->index.base() + 64);
			}
			TownSnapshot &ts = intel.town_snapshots[t->index.base()];
			ts.population = t->cache.population;
			ts.num_houses = t->cache.num_houses;
			ts.name = t->name;
			ts.xy = t->xy;
			ts.larger_town = t->larger_town;
			ts.exists = true;
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

	uint8_t local_side = GetLocalIntelligenceSide();

	/* Save previous currently_visible for local side to detect transitions */
	std::vector<uint8_t> prev_local_visible;
	if (local_side < MAX_MILITARY_SIDES && !_side_intelligence[local_side].currently_visible.empty()) {
		prev_local_visible = _side_intelligence[local_side].currently_visible;
	}

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

	/* Viewport dirty invalidation for local intelligence side */
	if (local_side < MAX_MILITARY_SIDES && !prev_local_visible.empty()) {
		const auto &now_visible = _side_intelligence[local_side].currently_visible;
		size_t count = std::min(prev_local_visible.size(), now_visible.size());
		for (size_t t = 0; t < count; t++) {
			if (prev_local_visible[t] != now_visible[t]) {
				MarkTileDirtyByTile(TileIndex{static_cast<uint>(t)});
			}
		}

		/* Explicitly update vehicle viewports for military disaster vehicles whose visibility may have changed */
		for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
			if (v->subtype == ST_MILITARY_TARGET_MARKER || v->subtype == ST_MILITARY_AIRCRAFT) {
				if (v->GetMilitarySide() != local_side) {
					v->UpdateViewport(true, false);
					if (IsValidTile(v->tile)) MarkTileDirtyByTile(v->tile);
				}
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

const IndustrySnapshot *MilitaryFog::GetIndustrySnapshotAtTile(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	const auto &intel = _side_intelligence[side];
	if (tile.base() >= intel.tile_to_industry_handle.size()) return nullptr;
	uint32_t handle = intel.tile_to_industry_handle[tile.base()];
	if (handle == 0) return nullptr;
	return GetIndustrySnapshotByHandle(side, handle);
}

const IndustrySnapshot *MilitaryFog::GetIndustrySnapshotByHandle(uint8_t side, uint32_t handle)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES || handle == 0) return nullptr;
	const auto &snaps = _side_intelligence[side].industry_snapshots;
	for (const auto &is : snaps) {
		if (is.exists && is.handle == handle) return &is;
	}
	return nullptr;
}

const IndustrySnapshot *MilitaryFog::GetIndustrySnapshotAtLocation(uint8_t side, TileIndex location)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES || location == INVALID_TILE) return nullptr;
	const auto &snaps = _side_intelligence[side].industry_snapshots;
	const IndustrySnapshot *fallback = nullptr;
	for (const auto &is : snaps) {
		if (is.exists && is.location == location) {
			if (std::find(is.observed_tiles.begin(), is.observed_tiles.end(), location) != is.observed_tiles.end()) {
				return &is;
			}
			fallback = &is;
		}
	}
	return fallback;
}

uint8_t MilitaryFog::GetIndustryProduction(uint8_t side, TileIndex location)
{
	const IndustrySnapshot *is = GetIndustrySnapshotAtLocation(side, location);
	return (is != nullptr) ? is->prod_level : 0;
}

const IndustrySnapshot *MilitaryFog::GetIndustrySnapshot(uint8_t side, IndustryID industry)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	if (Industry::IsValidID(industry)) {
		const Industry *ind = Industry::Get(industry);
		return GetIndustrySnapshotAtLocation(side, ind->location.tile);
	}
	return nullptr;
}

size_t MilitaryFog::GetIndustryTypeCount(uint8_t side, IndustryType type)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return 0;
	const auto &snaps = _side_intelligence[side].industry_snapshots;
	std::vector<TileIndex> counted_roots;
	for (const auto &is : snaps) {
		if (!is.exists || is.type != type || is.location == INVALID_TILE) continue;
		if (std::find(counted_roots.begin(), counted_roots.end(), is.location) == counted_roots.end()) {
			counted_roots.push_back(is.location);
		}
	}
	return counted_roots.size();
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
	this->prev_side = _active_side;
	Tile::active_base_tiles = intel.base_tiles.get();
	Tile::active_extended_tiles = intel.extended_tiles.get();
	_active_side = side;
	this->active = true;
}

ActiveSnapshotScope::~ActiveSnapshotScope()
{
	if (this->active) {
		Tile::active_base_tiles = this->prev_base;
		Tile::active_extended_tiles = this->prev_extended;
		_active_side = this->prev_side;
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

		/* Save town_snapshots */
		uint32_t num_towns = static_cast<uint32_t>(intel.town_snapshots.size());
		SlWriteByte(GB(num_towns, 24, 8));
		SlWriteByte(GB(num_towns, 16, 8));
		SlWriteByte(GB(num_towns, 8, 8));
		SlWriteByte(GB(num_towns, 0, 8));
		for (const auto &ts : intel.town_snapshots) {
			SlWriteByte(ts.exists ? 1 : 0);
			if (ts.exists) {
				SlWriteByte(GB(ts.population, 24, 8));
				SlWriteByte(GB(ts.population, 16, 8));
				SlWriteByte(GB(ts.population, 8, 8));
				SlWriteByte(GB(ts.population, 0, 8));

				SlWriteByte(GB(ts.num_houses, 24, 8));
				SlWriteByte(GB(ts.num_houses, 16, 8));
				SlWriteByte(GB(ts.num_houses, 8, 8));
				SlWriteByte(GB(ts.num_houses, 0, 8));

				SlWriteByte(GB(ts.xy.base(), 24, 8));
				SlWriteByte(GB(ts.xy.base(), 16, 8));
				SlWriteByte(GB(ts.xy.base(), 8, 8));
				SlWriteByte(GB(ts.xy.base(), 0, 8));

				SlWriteByte(ts.larger_town ? 1 : 0);

				uint16_t name_len = static_cast<uint16_t>(ts.name.size());
				SlWriteByte(GB(name_len, 8, 8));
				SlWriteByte(GB(name_len, 0, 8));
				for (char ch : ts.name) {
					SlWriteByte(static_cast<uint8_t>(ch));
				}
			}
		}

		/* Save industry_snapshots */
		SlWriteByte(GB(intel.next_industry_handle, 24, 8));
		SlWriteByte(GB(intel.next_industry_handle, 16, 8));
		SlWriteByte(GB(intel.next_industry_handle, 8, 8));
		SlWriteByte(GB(intel.next_industry_handle, 0, 8));

		uint32_t num_ind = static_cast<uint32_t>(intel.industry_snapshots.size());
		SlWriteByte(GB(num_ind, 24, 8));
		SlWriteByte(GB(num_ind, 16, 8));
		SlWriteByte(GB(num_ind, 8, 8));
		SlWriteByte(GB(num_ind, 0, 8));
		for (const auto &is : intel.industry_snapshots) {
			SlWriteByte(is.exists ? 1 : 0);
			if (is.exists) {
				SlWriteByte(GB(is.handle, 24, 8));
				SlWriteByte(GB(is.handle, 16, 8));
				SlWriteByte(GB(is.handle, 8, 8));
				SlWriteByte(GB(is.handle, 0, 8));

				SlWriteByte(is.type);

				SlWriteByte(GB(is.location.base(), 24, 8));
				SlWriteByte(GB(is.location.base(), 16, 8));
				SlWriteByte(GB(is.location.base(), 8, 8));
				SlWriteByte(GB(is.location.base(), 0, 8));

				SlWriteByte(is.prod_level);
				SlWriteByte(is.owner.base());
				SlWriteByte(to_underlying(is.colour));

				uint16_t name_len = static_cast<uint16_t>(is.name.size());
				SlWriteByte(GB(name_len, 8, 8));
				SlWriteByte(GB(name_len, 0, 8));
				for (char ch : is.name) {
					SlWriteByte(static_cast<uint8_t>(ch));
				}

				uint32_t num_observed = static_cast<uint32_t>(is.observed_tiles.size());
				SlWriteByte(GB(num_observed, 24, 8));
				SlWriteByte(GB(num_observed, 16, 8));
				SlWriteByte(GB(num_observed, 8, 8));
				SlWriteByte(GB(num_observed, 0, 8));
				for (TileIndex t : is.observed_tiles) {
					SlWriteByte(GB(t.base(), 24, 8));
					SlWriteByte(GB(t.base(), 16, 8));
					SlWriteByte(GB(t.base(), 8, 8));
					SlWriteByte(GB(t.base(), 0, 8));
				}
			}
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

		uint32_t num_towns = (static_cast<uint32_t>(SlReadByte()) << 24);
		num_towns |= (static_cast<uint32_t>(SlReadByte()) << 16);
		num_towns |= (static_cast<uint32_t>(SlReadByte()) << 8);
		num_towns |= static_cast<uint32_t>(SlReadByte());
		intel.town_snapshots.resize(num_towns);
		for (uint32_t j = 0; j < num_towns; j++) {
			TownSnapshot &ts = intel.town_snapshots[j];
			ts.exists = (SlReadByte() != 0);
			if (ts.exists) {
				uint32_t pop = (static_cast<uint32_t>(SlReadByte()) << 24);
				pop |= (static_cast<uint32_t>(SlReadByte()) << 16);
				pop |= (static_cast<uint32_t>(SlReadByte()) << 8);
				pop |= static_cast<uint32_t>(SlReadByte());
				ts.population = pop;

				uint32_t nh = (static_cast<uint32_t>(SlReadByte()) << 24);
				nh |= (static_cast<uint32_t>(SlReadByte()) << 16);
				nh |= (static_cast<uint32_t>(SlReadByte()) << 8);
				nh |= static_cast<uint32_t>(SlReadByte());
				ts.num_houses = nh;

				uint32_t xy = (static_cast<uint32_t>(SlReadByte()) << 24);
				xy |= (static_cast<uint32_t>(SlReadByte()) << 16);
				xy |= (static_cast<uint32_t>(SlReadByte()) << 8);
				xy |= static_cast<uint32_t>(SlReadByte());
				ts.xy = TileIndex{xy};

				ts.larger_town = (SlReadByte() != 0);

				uint16_t name_len = (static_cast<uint16_t>(SlReadByte()) << 8);
				name_len |= static_cast<uint16_t>(SlReadByte());
				ts.name.resize(name_len);
				for (uint16_t k = 0; k < name_len; k++) {
					ts.name[k] = static_cast<char>(SlReadByte());
				}
			} else {
				ts = TownSnapshot{};
			}
		}

		uint32_t next_h = (static_cast<uint32_t>(SlReadByte()) << 24);
		next_h |= (static_cast<uint32_t>(SlReadByte()) << 16);
		next_h |= (static_cast<uint32_t>(SlReadByte()) << 8);
		next_h |= static_cast<uint32_t>(SlReadByte());
		intel.next_industry_handle = next_h;

		uint32_t num_ind = (static_cast<uint32_t>(SlReadByte()) << 24);
		num_ind |= (static_cast<uint32_t>(SlReadByte()) << 16);
		num_ind |= (static_cast<uint32_t>(SlReadByte()) << 8);
		num_ind |= static_cast<uint32_t>(SlReadByte());
		intel.industry_snapshots.resize(num_ind);
		for (uint32_t j = 0; j < num_ind; j++) {
			IndustrySnapshot &is = intel.industry_snapshots[j];
			is.exists = (SlReadByte() != 0);
			if (is.exists) {
				uint32_t h = (static_cast<uint32_t>(SlReadByte()) << 24);
				h |= (static_cast<uint32_t>(SlReadByte()) << 16);
				h |= (static_cast<uint32_t>(SlReadByte()) << 8);
				h |= static_cast<uint32_t>(SlReadByte());
				is.handle = h;

				is.type = SlReadByte();

				uint32_t loc = (static_cast<uint32_t>(SlReadByte()) << 24);
				loc |= (static_cast<uint32_t>(SlReadByte()) << 16);
				loc |= (static_cast<uint32_t>(SlReadByte()) << 8);
				loc |= static_cast<uint32_t>(SlReadByte());
				is.location = TileIndex{loc};

				is.prod_level = SlReadByte();
				is.owner = Owner{SlReadByte()};
				is.colour = static_cast<Colours>(SlReadByte());

				uint16_t name_len = (static_cast<uint16_t>(SlReadByte()) << 8);
				name_len |= static_cast<uint16_t>(SlReadByte());
				is.name.resize(name_len);
				for (uint16_t k = 0; k < name_len; k++) {
					is.name[k] = static_cast<char>(SlReadByte());
				}

				uint32_t num_observed = (static_cast<uint32_t>(SlReadByte()) << 24);
				num_observed |= (static_cast<uint32_t>(SlReadByte()) << 16);
				num_observed |= (static_cast<uint32_t>(SlReadByte()) << 8);
				num_observed |= static_cast<uint32_t>(SlReadByte());
				is.observed_tiles.resize(num_observed);
				for (uint32_t ot = 0; ot < num_observed; ot++) {
					uint32_t t = (static_cast<uint32_t>(SlReadByte()) << 24);
					t |= (static_cast<uint32_t>(SlReadByte()) << 16);
					t |= (static_cast<uint32_t>(SlReadByte()) << 8);
					t |= static_cast<uint32_t>(SlReadByte());
					is.observed_tiles[ot] = TileIndex{t};
				}
			} else {
				is = IndustrySnapshot{};
			}
		}

		/* Reconstruct tile_to_industry_handle from snapshots */
		intel.tile_to_industry_handle.assign(map_size, 0);
		for (const auto &is : intel.industry_snapshots) {
			if (is.exists) {
				for (TileIndex t : is.observed_tiles) {
					if (t.base() < map_size) {
						intel.tile_to_industry_handle[t.base()] = is.handle;
					}
				}
			}
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

				const auto &intel = MilitaryFog::GetSideIntelligence(s);
				len += 4; // num_towns
				for (const auto &ts : intel.town_snapshots) {
					len += 1; // exists
					if (ts.exists) {
						len += 4 + 4 + 4 + 1 + 2 + ts.name.size();
					}
				}

				len += 4; // next_industry_handle
				len += 4; // num_industries
				for (const auto &is : intel.industry_snapshots) {
					len += 1; // exists
					if (is.exists) {
						len += 4 + 1 + 4 + 1 + 1 + 1 + 2 + is.name.size() + 4 + is.observed_tiles.size() * 4;
					}
				}
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
