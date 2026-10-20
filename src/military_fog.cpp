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
#include "core/string_builder.hpp"
#include "strings_func.h"
#include "table/control_codes.h"
#include "table/strings.h"
#include "landscape.h"
#include "settings_type.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "station_map.h"
#include <algorithm>
#include "core/format.hpp"
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

/**
 * Get safe cached name of a town without triggering unloaded string table assertion in headless/test environments.
 * @param t The town to query.
 * @return The cached town name, or empty string.
 */
static std::string GetSafeTownName(const Town *t)
{
	if (t == nullptr) return "";
	if (!t->cached_name.empty()) return t->cached_name;
	if (!t->name.empty()) return t->name;
	if (_current_language != nullptr) {
		return t->GetCachedName();
	}
	return t->name;
}

std::string TownSnapshot::GetViewportString(bool small) const
{
	if (small) return this->name;
	std::string res;
	StringBuilder sb(res);
	if (_settings_client.gui.population_in_label) {
		std::string pop = (_current_language != nullptr) ? GetString(STR_JUST_COMMA, this->population) : fmt::format("{}", this->population);
		sb += this->name;
		sb += " (";
		sb += pop;
		sb += ")";
		if (this->larger_town) {
			sb += " ";
			sb.PutUtf8(SCC_CITY);
		}
		return res;
	}
	sb += this->name;
	if (this->larger_town) {
		sb += " ";
		sb.PutUtf8(SCC_CITY);
	}
	return res;
}

void TownSnapshot::UpdateVirtCoord()
{
	if (!this->exists || !IsValidTile(this->xy)) return;

	Point pt = RemapCoords2(TileX(this->xy) * TILE_SIZE, TileY(this->xy) * TILE_SIZE);

	std::string str_normal = this->GetViewportString(false);
	std::string str_small = this->GetViewportString(true);

	if (_current_language != nullptr) {
		this->sign.UpdatePosition(pt.x, pt.y - 24 * ZOOM_BASE, str_normal, str_small);
	} else {
		this->sign.center = pt.x;
		this->sign.top = pt.y - 24 * ZOOM_BASE;
		this->sign.width_normal = static_cast<uint16_t>(std::max<size_t>(str_normal.size() * 8, 40));
		this->sign.width_small = static_cast<uint16_t>(std::max<size_t>(str_small.size() * 6, 20));
	}
}

/**
 * Get safe cached name of a station or waypoint without triggering unloaded string table assertion in headless/test environments.
 * @param st The base station to query.
 * @return The cached station name, or empty string.
 */
static std::string GetSafeStationName(const BaseStation *st)
{
	if (st == nullptr) return "";
	if (!st->cached_name.empty()) return st->cached_name;
	if (!st->name.empty()) return st->name;
	if (_current_language != nullptr) {
		return st->GetCachedName();
	}
	return st->name;
}

std::string StationSnapshot::GetViewportString(bool small) const
{
	if (small || this->is_waypoint) {
		return this->name;
	}

	bool has_icons = this->facilities.Any({StationFacility::Train, StationFacility::TruckStop, StationFacility::BusStop, StationFacility::Dock, StationFacility::Airport});
	if (!has_icons) {
		return this->name;
	}

	std::string res = this->name;
	StringBuilder sb(res);
	sb += " ";
	if (this->facilities.Test(StationFacility::Train)) sb.PutUtf8(SCC_TRAIN);
	if (this->facilities.Test(StationFacility::TruckStop)) sb.PutUtf8(SCC_LORRY);
	if (this->facilities.Test(StationFacility::BusStop)) sb.PutUtf8(SCC_BUS);
	if (this->facilities.Test(StationFacility::Dock)) sb.PutUtf8(SCC_SHIP);
	if (this->facilities.Test(StationFacility::Airport)) sb.PutUtf8(SCC_PLANE);
	return res;
}

void StationSnapshot::UpdateVirtCoord()
{
	if (!this->exists || !IsValidTile(this->xy)) return;

	Point pt = RemapCoords2(TileX(this->xy) * TILE_SIZE, TileY(this->xy) * TILE_SIZE);
	pt.y -= 32 * ZOOM_BASE;
	if (this->is_oilrig) pt.y -= 16 * ZOOM_BASE;

	std::string str_normal = this->GetViewportString(false);
	std::string str_small = this->GetViewportString(true);

	if (_current_language != nullptr) {
		this->sign.UpdatePosition(pt.x, pt.y, str_normal, str_small);
	} else {
		this->sign.center = pt.x;
		this->sign.top = pt.y;
		this->sign.width_normal = static_cast<uint16_t>(std::max<size_t>(str_normal.size() * 8, 40));
		this->sign.width_small = static_cast<uint16_t>(std::max<size_t>(str_small.size() * 6, 20));
	}
}

void StationSnapshot::SetTileContribution(TileIndex tile, StationFacilities fac)
{
	auto it = std::find(this->observed_tiles.begin(), this->observed_tiles.end(), tile);
	if (it != this->observed_tiles.end()) {
		size_t idx = std::distance(this->observed_tiles.begin(), it);
		if (idx < this->tile_facilities.size()) {
			this->tile_facilities[idx] = fac;
		}
	} else {
		this->observed_tiles.push_back(tile);
		this->tile_facilities.push_back(fac);
	}
	this->RecomputeAggregateFacilities();
}

void StationSnapshot::RemoveTileContribution(TileIndex tile)
{
	auto it = std::find(this->observed_tiles.begin(), this->observed_tiles.end(), tile);
	if (it != this->observed_tiles.end()) {
		size_t idx = std::distance(this->observed_tiles.begin(), it);
		this->observed_tiles.erase(it);
		if (idx < this->tile_facilities.size()) {
			this->tile_facilities.erase(this->tile_facilities.begin() + idx);
		}
	}
	this->RecomputeAggregateFacilities();
}

void StationSnapshot::RecomputeAggregateFacilities()
{
	this->facilities.Reset();
	if (this->is_waypoint) {
		this->facilities.Set(StationFacility::Waypoint);
	}
	bool has_oilrig = false;
	for (size_t i = 0; i < this->observed_tiles.size(); ++i) {
		if (i < this->tile_facilities.size()) {
			this->facilities.Set(this->tile_facilities[i]);
			if (this->tile_facilities[i].Test(StationFacility::Airport) && this->tile_facilities[i].Test(StationFacility::Dock)) {
				has_oilrig = true;
			}
		}
	}
	this->is_oilrig = has_oilrig;
}

static StationFacilities GetTileStationFacilities(TileIndex tile)
{
	if (!IsTileType(tile, TileType::Station)) return StationFacilities{};

	StationType st = GetStationType(tile);
	switch (st) {
		case StationType::Rail:
			return StationFacility::Train;
		case StationType::Airport:
			return StationFacility::Airport;
		case StationType::Truck:
			return StationFacility::TruckStop;
		case StationType::Bus:
			return StationFacility::BusStop;
		case StationType::Oilrig:
			return {StationFacility::Airport, StationFacility::Dock};
		case StationType::Dock:
			return StationFacility::Dock;
		case StationType::Buoy:
			return StationFacility::Waypoint;
		case StationType::RailWaypoint:
			return StationFacility::Waypoint;
		case StationType::RoadWaypoint:
			return {StationFacility::BusStop, StationFacility::TruckStop};
		default:
			return StationFacilities{};
	}
}

void SideIntelligence::Allocate(size_t map_size)
{
	this->is_scouted.assign(map_size, 0);
	this->currently_visible.assign(map_size, 0);
	this->tile_to_industry_handle.assign(map_size, 0);
	this->tile_to_station_handle.assign(map_size, 0);
	this->base_tiles = std::make_unique<Tile::TileBase[]>(map_size);
	this->extended_tiles = std::make_unique<Tile::TileExtended[]>(map_size);
	this->town_snapshots.clear();
	this->station_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = static_cast<uint32_t>(map_size);
	this->next_industry_handle = 1;
	this->next_town_handle = 1;
	this->next_station_handle = 1;
}

void SideIntelligence::Reset()
{
	this->is_scouted.clear();
	this->currently_visible.clear();
	this->tile_to_industry_handle.clear();
	this->tile_to_station_handle.clear();
	this->base_tiles.reset();
	this->extended_tiles.reset();
	this->town_snapshots.clear();
	this->station_snapshots.clear();
	this->industry_snapshots.clear();
	this->contacts.clear();
	this->never_scouted_count = 0;
	this->next_industry_handle = 1;
	this->next_town_handle = 1;
	this->next_station_handle = 1;
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

		/* Snapshot starting towns with location-anchored handles */
		_side_intelligence[s].town_snapshots.clear();
		uint32_t town_h = 1;
		for (const Town *t : Town::Iterate()) {
			TownSnapshot ts;
			ts.handle = town_h++;
			ts.original_id = t->index;
			ts.population = t->cache.population;
			ts.num_houses = t->cache.num_houses;
			ts.name = GetSafeTownName(t);
			ts.xy = t->xy;
			ts.larger_town = t->larger_town;
			ts.exists = true;
			ts.UpdateVirtCoord();
			_side_intelligence[s].town_snapshots.push_back(std::move(ts));
		}
		_side_intelligence[s].next_town_handle = town_h;

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

		/* Snapshot starting stations and waypoints with location-anchored handles and footprint tiles */
		_side_intelligence[s].station_snapshots.clear();
		uint32_t station_h = 1;
		for (const BaseStation *st : BaseStation::Iterate()) {
			uint32_t handle = station_h++;
			StationSnapshot ss;
			ss.handle = handle;
			ss.instance_id = st->instance_id;
			ss.original_id = st->index;
			ss.is_waypoint = !Station::IsExpected(st);
			ss.xy = st->xy;
			ss.name = GetSafeStationName(st);
			ss.owner = st->owner;
			ss.colour = (st->owner == OWNER_NONE) ? Colours::Grey : _company_colours[st->owner];
			ss.exists = true;

			for (TileIndex cur_tile : st->spread) {
				if (IsTileType(cur_tile, TileType::Station) && GetStationIndex(cur_tile) == st->index) {
					StationFacilities fac = GetTileStationFacilities(cur_tile);
					ss.SetTileContribution(cur_tile, fac);
					_side_intelligence[s].tile_to_station_handle[cur_tile.base()] = handle;
				}
			}
			if (std::find(ss.observed_tiles.begin(), ss.observed_tiles.end(), st->xy) == ss.observed_tiles.end() && IsValidTile(st->xy)) {
				StationFacilities fac = (IsTileType(st->xy, TileType::Station) && GetStationIndex(st->xy) == st->index) ? GetTileStationFacilities(st->xy) : StationFacilities{};
				ss.SetTileContribution(st->xy, fac);
				_side_intelligence[s].tile_to_station_handle[st->xy.base()] = handle;
			}
			ss.UpdateVirtCoord();
			_side_intelligence[s].station_snapshots.push_back(std::move(ss));
		}
		_side_intelligence[s].next_station_handle = station_h;

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

void MilitaryFog::ResetStationInstanceCounter()
{
	InitializeStations();
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

	/* Refresh town snapshot if tile is a town center or belongs to a town */
	const Town *live_town = nullptr;
	for (const Town *t : Town::Iterate()) {
		if (t->xy == tile) {
			live_town = t;
			break;
		}
	}
	if (live_town == nullptr && IsTileType(tile, TileType::House)) {
		TownID tid = GetTownIndex(tile);
		if (Town::IsValidID(tid)) {
			live_town = Town::Get(tid);
		}
	}

	if (live_town != nullptr) {
		TownSnapshot *match = nullptr;
		for (auto &ts : intel.town_snapshots) {
			if (ts.xy == live_town->xy) {
				match = &ts;
				break;
			}
		}
		if (match == nullptr) {
			TownSnapshot ts;
			ts.handle = intel.next_town_handle++;
			ts.original_id = live_town->index;
			ts.xy = live_town->xy;
			ts.name = GetSafeTownName(live_town);
			ts.population = live_town->cache.population;
			ts.num_houses = live_town->cache.num_houses;
			ts.larger_town = live_town->larger_town;
			ts.exists = true;
			ts.UpdateVirtCoord();
			intel.town_snapshots.push_back(std::move(ts));
		} else {
			match->name = GetSafeTownName(live_town);
			match->population = live_town->cache.population;
			match->num_houses = live_town->cache.num_houses;
			match->larger_town = live_town->larger_town;
			match->original_id = live_town->index;
			match->exists = true;
			match->UpdateVirtCoord();
		}
	} else {
		/* Tile is not part of any live town. If it was a known town center, discover town deletion */
		for (auto &ts : intel.town_snapshots) {
			if (ts.xy == tile && ts.exists) {
				bool live_found = false;
				for (const Town *t : Town::Iterate()) {
					if (t->xy == tile) {
						live_found = true;
						break;
					}
				}
				if (!live_found) {
					ts.exists = false;
				}
			}
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
					prev_snap->colour == i->random_colour && prev_snap->name == GetSafeIndustryName(i) &&
					prev_snap->owner == i->owner) {
				if (tile == root) {
					prev_snap->prod_level = i->prod_level;
					prev_snap->owner = i->owner;
				}
				if (std::find(prev_snap->observed_tiles.begin(), prev_snap->observed_tiles.end(), tile) == prev_snap->observed_tiles.end()) {
					prev_snap->observed_tiles.push_back(tile);
				}
				if (tile.base() < intel.tile_to_industry_handle.size()) {
					intel.tile_to_industry_handle[tile.base()] = prev_snap->handle;
				}
			} else {
				/* Tile changed or is new: detach tile from previous snapshot without mutating remaining hidden tiles */
				uint8_t prev_prod = (prev_snap != nullptr) ? prev_snap->prod_level : 0;

				if (prev_snap != nullptr) {
					auto it = std::find(prev_snap->observed_tiles.begin(), prev_snap->observed_tiles.end(), tile);
					if (it != prev_snap->observed_tiles.end()) {
						prev_snap->observed_tiles.erase(it);
					}
					if (prev_snap->observed_tiles.empty()) {
						prev_snap->exists = false;
					}
				}
				prev_snap = nullptr; // Raw pointer must not be accessed across potential vector growth

				/* Find if an existing active snapshot already represents this new live industry appearance at root */
				IndustrySnapshot *snap = nullptr;
				for (auto &existing : intel.industry_snapshots) {
					if (existing.exists && existing.location == root && existing.type == i->type &&
							existing.colour == i->random_colour && existing.name == GetSafeIndustryName(i) &&
							existing.owner == i->owner) {
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
					snap->owner = i->owner;
					snap->prod_level = (tile == root) ? i->prod_level : prev_prod;
				}
				if (tile == root) {
					snap->prod_level = i->prod_level;
					snap->owner = i->owner;
				}
				snap->exists = true;

				if (std::find(snap->observed_tiles.begin(), snap->observed_tiles.end(), tile) == snap->observed_tiles.end()) {
					snap->observed_tiles.push_back(tile);
				}
				if (tile.base() < intel.tile_to_industry_handle.size()) {
					intel.tile_to_industry_handle[tile.base()] = snap->handle;
				}
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

	/* Refresh station snapshot if tile belongs to a station or waypoint */
	if (IsTileType(tile, TileType::Station)) {
		StationID sid = GetStationIndex(tile);
		const BaseStation *st = BaseStation::GetIfValid(sid);
		if (st != nullptr) {
			TileIndex sign_xy = st->xy;
			bool is_wp = !Station::IsExpected(st);
			std::string st_name = GetSafeStationName(st);
			Owner st_owner = st->owner;
			StationFacilities tile_fac = GetTileStationFacilities(tile);

			uint32_t prev_handle = (tile.base() < intel.tile_to_station_handle.size()) ? intel.tile_to_station_handle[tile.base()] : 0;
			StationSnapshot *prev_snap = (prev_handle != 0) ? const_cast<StationSnapshot *>(GetStationSnapshotByHandle(side, prev_handle)) : nullptr;

			/* If previous snapshot matches current live station instance, update it */
			if (prev_snap != nullptr && prev_snap->exists && prev_snap->instance_id == st->instance_id) {
				prev_snap->name = st_name;
				prev_snap->colour = (st_owner == OWNER_NONE) ? Colours::Grey : _company_colours[st_owner];
				prev_snap->xy = sign_xy;
				prev_snap->SetTileContribution(tile, tile_fac);
				if (tile.base() < intel.tile_to_station_handle.size()) {
					intel.tile_to_station_handle[tile.base()] = prev_snap->handle;
				}
				prev_snap->UpdateVirtCoord();
			} else {
				/* Tile changed, is replaced, or is new: detach tile from previous snapshot without mutating remaining hidden tiles */
				if (prev_snap != nullptr && prev_snap->exists) {
					prev_snap->RemoveTileContribution(tile);
					if (prev_snap->observed_tiles.empty()) {
						prev_snap->exists = false;
					}
					prev_snap->UpdateVirtCoord();
				}
				prev_snap = nullptr; // Raw pointer must not be accessed across potential vector growth

				/* Find if an existing active snapshot already represents this live station instance (e.g. disjoint components) */
				StationSnapshot *snap = nullptr;
				for (auto &existing : intel.station_snapshots) {
					if (existing.exists && existing.instance_id == st->instance_id) {
						snap = &existing;
						break;
					}
				}
				if (snap == nullptr) {
					uint32_t handle = intel.next_station_handle++;
					intel.station_snapshots.emplace_back();
					snap = &intel.station_snapshots.back();
					snap->handle = handle;
					snap->instance_id = st->instance_id;
					snap->original_id = sid;
					snap->is_waypoint = is_wp;
					snap->xy = sign_xy;
					snap->name = st_name;
					snap->owner = st_owner;
				}
				snap->name = st_name;
				snap->colour = (st_owner == OWNER_NONE) ? Colours::Grey : _company_colours[st_owner];
				snap->xy = sign_xy;
				snap->exists = true;
				snap->SetTileContribution(tile, tile_fac);
				if (tile.base() < intel.tile_to_station_handle.size()) {
					intel.tile_to_station_handle[tile.base()] = snap->handle;
				}
				snap->UpdateVirtCoord();
			}
		}
	} else {
		/* Tile is no longer a station or waypoint */
		uint32_t prev_handle = (tile.base() < intel.tile_to_station_handle.size()) ? intel.tile_to_station_handle[tile.base()] : 0;
		if (prev_handle != 0) {
			intel.tile_to_station_handle[tile.base()] = 0;
			for (auto &existing : intel.station_snapshots) {
				if (existing.handle == prev_handle) {
					existing.RemoveTileContribution(tile);
					if (existing.observed_tiles.empty()) {
						existing.exists = false;
					}
					existing.UpdateVirtCoord();
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
			TownSnapshot *match = nullptr;
			for (auto &ts : intel.town_snapshots) {
				if (ts.xy == t->xy) {
					match = &ts;
					break;
				}
			}
			if (match == nullptr) {
				TownSnapshot ts;
				ts.handle = intel.next_town_handle++;
				ts.original_id = t->index;
				ts.xy = t->xy;
				ts.name = GetSafeTownName(t);
				ts.population = t->cache.population;
				ts.num_houses = t->cache.num_houses;
				ts.larger_town = t->larger_town;
				ts.exists = true;
				ts.UpdateVirtCoord();
				intel.town_snapshots.push_back(std::move(ts));
			} else {
				match->name = GetSafeTownName(t);
				match->population = t->cache.population;
				match->num_houses = t->cache.num_houses;
				match->larger_town = t->larger_town;
				match->original_id = t->index;
				match->exists = true;
				match->UpdateVirtCoord();
			}
		}
	}

	/* Also check if any known snapshot in radius has ceased to exist */
	for (auto &ts : intel.town_snapshots) {
		if (!ts.exists) continue;
		if (DistanceSquare(ts.xy, center_tile) <= static_cast<uint>(r * r)) {
			bool live_found = false;
			for (const Town *t : Town::Iterate()) {
				if (t->xy == ts.xy) {
					live_found = true;
					break;
				}
			}
			if (!live_found) {
				ts.exists = false;
			}
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
	if (Town::IsValidID(town)) {
		TileIndex xy = Town::Get(town)->xy;
		for (const auto &ts : snaps) {
			if (ts.exists && ts.xy == xy) return &ts;
		}
	}
	for (const auto &ts : snaps) {
		if (ts.exists && ts.original_id == town) return &ts;
	}
	return nullptr;
}

const TownSnapshot *MilitaryFog::GetTownSnapshotAtTile(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	const auto &snaps = _side_intelligence[side].town_snapshots;
	for (const auto &ts : snaps) {
		if (ts.exists && ts.xy == tile) return &ts;
	}
	return nullptr;
}

void MilitaryFog::UpdateAllTownSnapshotVirtCoords()
{
	if (!_military_fog_active) return;
	for (uint8_t s = 0; s < MAX_MILITARY_SIDES; s++) {
		for (auto &ts : _side_intelligence[s].town_snapshots) {
			if (ts.exists) {
				ts.UpdateVirtCoord();
			}
		}
	}
}

const StationSnapshot *MilitaryFog::GetStationSnapshotByHandle(uint8_t side, uint32_t handle)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES || handle == 0) return nullptr;
	const auto &snaps = _side_intelligence[side].station_snapshots;
	for (const auto &ss : snaps) {
		if (ss.handle == handle) return &ss;
	}
	return nullptr;
}

const StationSnapshot *MilitaryFog::GetStationSnapshotAtTile(uint8_t side, TileIndex tile)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES || !IsValidTile(tile)) return nullptr;
	const auto &intel = _side_intelligence[side];
	if (tile.base() < intel.tile_to_station_handle.size()) {
		uint32_t handle = intel.tile_to_station_handle[tile.base()];
		if (handle != 0) {
			const StationSnapshot *ss = GetStationSnapshotByHandle(side, handle);
			if (ss != nullptr && ss->exists) return ss;
		}
	}
	for (const auto &ss : intel.station_snapshots) {
		if (ss.exists && ss.xy == tile) return &ss;
	}
	return nullptr;
}

const StationSnapshot *MilitaryFog::GetStationSnapshot(uint8_t side, StationID station)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES) return nullptr;
	for (const auto &ss : _side_intelligence[side].station_snapshots) {
		if (ss.exists && ss.original_id == station) return &ss;
	}
	return nullptr;
}

const StationSnapshot *MilitaryFog::GetStationSnapshotByInstance(uint8_t side, uint32_t instance_id)
{
	if (!_military_fog_active || side >= MAX_MILITARY_SIDES || instance_id == 0) return nullptr;
	for (const auto &ss : _side_intelligence[side].station_snapshots) {
		if (ss.exists && ss.instance_id == instance_id) return &ss;
	}
	return nullptr;
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
		SlWriteByte(GB(intel.next_town_handle, 24, 8));
		SlWriteByte(GB(intel.next_town_handle, 16, 8));
		SlWriteByte(GB(intel.next_town_handle, 8, 8));
		SlWriteByte(GB(intel.next_town_handle, 0, 8));

		uint32_t num_towns = static_cast<uint32_t>(intel.town_snapshots.size());
		SlWriteByte(GB(num_towns, 24, 8));
		SlWriteByte(GB(num_towns, 16, 8));
		SlWriteByte(GB(num_towns, 8, 8));
		SlWriteByte(GB(num_towns, 0, 8));
		for (const auto &ts : intel.town_snapshots) {
			SlWriteByte(ts.exists ? 1 : 0);
			if (ts.exists) {
				SlWriteByte(GB(ts.handle, 24, 8));
				SlWriteByte(GB(ts.handle, 16, 8));
				SlWriteByte(GB(ts.handle, 8, 8));
				SlWriteByte(GB(ts.handle, 0, 8));

				SlWriteByte(GB(ts.original_id.base(), 8, 8));
				SlWriteByte(GB(ts.original_id.base(), 0, 8));

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

		/* Save station_snapshots */
		SlWriteByte(GB(intel.next_station_handle, 24, 8));
		SlWriteByte(GB(intel.next_station_handle, 16, 8));
		SlWriteByte(GB(intel.next_station_handle, 8, 8));
		SlWriteByte(GB(intel.next_station_handle, 0, 8));

		uint32_t num_st = static_cast<uint32_t>(intel.station_snapshots.size());
		SlWriteByte(GB(num_st, 24, 8));
		SlWriteByte(GB(num_st, 16, 8));
		SlWriteByte(GB(num_st, 8, 8));
		SlWriteByte(GB(num_st, 0, 8));
		for (const auto &ss : intel.station_snapshots) {
			SlWriteByte(ss.exists ? 1 : 0);
			if (ss.exists) {
				SlWriteByte(GB(ss.handle, 24, 8));
				SlWriteByte(GB(ss.handle, 16, 8));
				SlWriteByte(GB(ss.handle, 8, 8));
				SlWriteByte(GB(ss.handle, 0, 8));

				SlWriteByte(GB(ss.instance_id, 24, 8));
				SlWriteByte(GB(ss.instance_id, 16, 8));
				SlWriteByte(GB(ss.instance_id, 8, 8));
				SlWriteByte(GB(ss.instance_id, 0, 8));

				uint16_t orig_id = ss.original_id.base();
				SlWriteByte(GB(orig_id, 8, 8));
				SlWriteByte(GB(orig_id, 0, 8));

				SlWriteByte(ss.is_waypoint ? 1 : 0);
				SlWriteByte(ss.is_oilrig ? 1 : 0);

				SlWriteByte(GB(ss.xy.base(), 24, 8));
				SlWriteByte(GB(ss.xy.base(), 16, 8));
				SlWriteByte(GB(ss.xy.base(), 8, 8));
				SlWriteByte(GB(ss.xy.base(), 0, 8));

				SlWriteByte(ss.owner.base());
				SlWriteByte(ss.facilities.base());
				SlWriteByte(to_underlying(ss.colour));

				uint16_t name_len = static_cast<uint16_t>(ss.name.size());
				SlWriteByte(GB(name_len, 8, 8));
				SlWriteByte(GB(name_len, 0, 8));
				for (char ch : ss.name) {
					SlWriteByte(static_cast<uint8_t>(ch));
				}

				uint32_t num_observed = static_cast<uint32_t>(ss.observed_tiles.size());
				SlWriteByte(GB(num_observed, 24, 8));
				SlWriteByte(GB(num_observed, 16, 8));
				SlWriteByte(GB(num_observed, 8, 8));
				SlWriteByte(GB(num_observed, 0, 8));
				for (size_t k = 0; k < ss.observed_tiles.size(); k++) {
					TileIndex t = ss.observed_tiles[k];
					SlWriteByte(GB(t.base(), 24, 8));
					SlWriteByte(GB(t.base(), 16, 8));
					SlWriteByte(GB(t.base(), 8, 8));
					SlWriteByte(GB(t.base(), 0, 8));
					uint8_t fac = (k < ss.tile_facilities.size()) ? ss.tile_facilities[k].base() : 0;
					SlWriteByte(fac);
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

		/* Load town_snapshots */
		uint32_t next_town_h = (static_cast<uint32_t>(SlReadByte()) << 24);
		next_town_h |= (static_cast<uint32_t>(SlReadByte()) << 16);
		next_town_h |= (static_cast<uint32_t>(SlReadByte()) << 8);
		next_town_h |= static_cast<uint32_t>(SlReadByte());
		intel.next_town_handle = next_town_h;

		uint32_t num_towns = (static_cast<uint32_t>(SlReadByte()) << 24);
		num_towns |= (static_cast<uint32_t>(SlReadByte()) << 16);
		num_towns |= (static_cast<uint32_t>(SlReadByte()) << 8);
		num_towns |= static_cast<uint32_t>(SlReadByte());
		intel.town_snapshots.resize(num_towns);
		for (uint32_t j = 0; j < num_towns; j++) {
			TownSnapshot &ts = intel.town_snapshots[j];
			ts.exists = (SlReadByte() != 0);
			if (ts.exists) {
				uint32_t h = (static_cast<uint32_t>(SlReadByte()) << 24);
				h |= (static_cast<uint32_t>(SlReadByte()) << 16);
				h |= (static_cast<uint32_t>(SlReadByte()) << 8);
				h |= static_cast<uint32_t>(SlReadByte());
				ts.handle = h;

				uint16_t orig_id = (static_cast<uint16_t>(SlReadByte()) << 8);
				orig_id |= static_cast<uint16_t>(SlReadByte());
				ts.original_id = TownID{orig_id};

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
				ts.UpdateVirtCoord();
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

		/* Load station_snapshots */
		uint32_t next_st_h = (static_cast<uint32_t>(SlReadByte()) << 24);
		next_st_h |= (static_cast<uint32_t>(SlReadByte()) << 16);
		next_st_h |= (static_cast<uint32_t>(SlReadByte()) << 8);
		next_st_h |= static_cast<uint32_t>(SlReadByte());
		intel.next_station_handle = next_st_h;

		uint32_t num_st = (static_cast<uint32_t>(SlReadByte()) << 24);
		num_st |= (static_cast<uint32_t>(SlReadByte()) << 16);
		num_st |= (static_cast<uint32_t>(SlReadByte()) << 8);
		num_st |= static_cast<uint32_t>(SlReadByte());
		intel.station_snapshots.resize(num_st);
		for (uint32_t j = 0; j < num_st; j++) {
			StationSnapshot &ss = intel.station_snapshots[j];
			ss.exists = (SlReadByte() != 0);
			if (ss.exists) {
				uint32_t h = (static_cast<uint32_t>(SlReadByte()) << 24);
				h |= (static_cast<uint32_t>(SlReadByte()) << 16);
				h |= (static_cast<uint32_t>(SlReadByte()) << 8);
				h |= static_cast<uint32_t>(SlReadByte());
				ss.handle = h;

				uint32_t inst_id = (static_cast<uint32_t>(SlReadByte()) << 24);
				inst_id |= (static_cast<uint32_t>(SlReadByte()) << 16);
				inst_id |= (static_cast<uint32_t>(SlReadByte()) << 8);
				inst_id |= static_cast<uint32_t>(SlReadByte());
				ss.instance_id = inst_id;

				uint16_t orig_id = (static_cast<uint16_t>(SlReadByte()) << 8);
				orig_id |= static_cast<uint16_t>(SlReadByte());
				ss.original_id = StationID{orig_id};

				ss.is_waypoint = (SlReadByte() != 0);
				ss.is_oilrig = (SlReadByte() != 0);

				uint32_t xy = (static_cast<uint32_t>(SlReadByte()) << 24);
				xy |= (static_cast<uint32_t>(SlReadByte()) << 16);
				xy |= (static_cast<uint32_t>(SlReadByte()) << 8);
				xy |= static_cast<uint32_t>(SlReadByte());
				ss.xy = TileIndex{xy};

				ss.owner = Owner{SlReadByte()};
				ss.facilities = static_cast<StationFacilities>(SlReadByte());
				ss.colour = static_cast<Colours>(SlReadByte());

				uint16_t name_len = (static_cast<uint16_t>(SlReadByte()) << 8);
				name_len |= static_cast<uint16_t>(SlReadByte());
				ss.name.resize(name_len);
				for (uint16_t k = 0; k < name_len; k++) {
					ss.name[k] = static_cast<char>(SlReadByte());
				}

				uint32_t num_observed = (static_cast<uint32_t>(SlReadByte()) << 24);
				num_observed |= (static_cast<uint32_t>(SlReadByte()) << 16);
				num_observed |= (static_cast<uint32_t>(SlReadByte()) << 8);
				num_observed |= static_cast<uint32_t>(SlReadByte());
				ss.observed_tiles.resize(num_observed);
				ss.tile_facilities.resize(num_observed);
				for (uint32_t ot = 0; ot < num_observed; ot++) {
					uint32_t t = (static_cast<uint32_t>(SlReadByte()) << 24);
					t |= (static_cast<uint32_t>(SlReadByte()) << 16);
					t |= (static_cast<uint32_t>(SlReadByte()) << 8);
					t |= static_cast<uint32_t>(SlReadByte());
					ss.observed_tiles[ot] = TileIndex{t};
					uint8_t fac = SlReadByte();
					ss.tile_facilities[ot] = static_cast<StationFacilities>(fac);
				}
				ss.RecomputeAggregateFacilities();
				ss.UpdateVirtCoord();
			} else {
				ss = StationSnapshot{};
			}
		}

		/* Reconstruct tile_to_station_handle from snapshots */
		intel.tile_to_station_handle.assign(map_size, 0);
		for (const auto &ss : intel.station_snapshots) {
			if (ss.exists) {
				_next_station_instance_id = std::max(_next_station_instance_id, ss.instance_id);
				for (TileIndex t : ss.observed_tiles) {
					if (t.base() < map_size) {
						intel.tile_to_station_handle[t.base()] = ss.handle;
					}
				}
			}
		}
	}

	for (const BaseStation *st : BaseStation::Iterate()) {
		_next_station_instance_id = std::max(_next_station_instance_id, st->instance_id);
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
				len += 4; // next_town_handle
				len += 4; // num_towns
				for (const auto &ts : intel.town_snapshots) {
					len += 1; // exists
					if (ts.exists) {
						len += 4 + 2 + 4 + 4 + 4 + 1 + 2 + ts.name.size(); // handle (4) + original_id (2) + pop (4) + num_houses (4) + xy (4) + larger_town (1) + name_len (2) + name
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

				len += 4; // next_station_handle
				len += 4; // num_stations
				for (const auto &ss : intel.station_snapshots) {
					len += 1; // exists
					if (ss.exists) {
						len += 4 + 4 + 2 + 1 + 1 + 4 + 1 + 1 + 1 + 2 + ss.name.size() + 4 + ss.observed_tiles.size() * 5;
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
