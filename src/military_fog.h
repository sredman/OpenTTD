/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file military_fog.h Fog of war and knowledge-limited military scouting declarations. */

#ifndef MILITARY_FOG_H
#define MILITARY_FOG_H

#include "map_func.h"
#include "vehicle_base.h"
#include "company_base.h"
#include "town.h"
#include "industry.h"
#include <vector>
#include <string>
#include <memory>

/** Maximum number of military sides supported for fog of war. */
static constexpr uint8_t MAX_MILITARY_SIDES = 2;

/** Scouting radius in tiles revealed by military aircraft. */
static constexpr uint MILITARY_SCOUT_RADIUS = 5;

/** Fog of war visibility status per tile for a given side. */
enum FogStatus : uint8_t {
	FOG_NEVER_SCOUTED = 0,      ///< Location has never been scouted by this side.
	FOG_PREVIOUSLY_SCOUTED = 1, ///< Location was previously scouted, but is not currently visible.
	FOG_CURRENTLY_VISIBLE = 2,  ///< Location is currently inside the scouting radius of an active unit.
};

/** Stored stationary snapshot of a town. */
struct TownSnapshot {
	uint32_t population = 0;   ///< Last-observed population of the town.
	uint32_t num_houses = 0;   ///< Last-observed number of houses in the town.
	std::string name{};        ///< Last-observed town name.
	bool exists = false;       ///< Whether the town existed when last observed.
};

/** Stored stationary snapshot of an industry. */
struct IndustrySnapshot {
	IndustryType type = 0;              ///< Industry type ID.
	TileIndex location = INVALID_TILE; ///< Industry location center/origin tile.
	uint8_t prod_level = 0;            ///< Last-observed production level.
	bool exists = false;               ///< Whether the industry existed when last observed.
};

/** Stored internally remembered enemy contact. */
struct MilitaryContact {
	VehicleID vehicle_id = VehicleID::Invalid(); ///< Contact identifier.
	int last_x = 0;                              ///< Last-observed X pixel coordinate.
	int last_y = 0;                              ///< Last-observed Y pixel coordinate.
	TileIndex last_tile = INVALID_TILE;          ///< Last-observed tile index.
	uint8_t side = 0;                            ///< Military side of the enemy unit.
	bool currently_visible = false;              ///< Whether this contact is currently seen.
};

/** Complete intelligence state for a single military side. */
struct SideIntelligence {
	std::vector<uint8_t> is_scouted;                      ///< 1 if explored at least once, 0 if never scouted.
	std::vector<uint8_t> currently_visible;               ///< 1 if currently in scouting radius, 0 otherwise.
	std::unique_ptr<Tile::TileBase[]> base_tiles;         ///< Stationary snapshot of TileBase data.
	std::unique_ptr<Tile::TileExtended[]> extended_tiles; ///< Stationary snapshot of TileExtended data.
	std::vector<TownSnapshot> town_snapshots;             ///< Snapshots of towns indexed by TownID.
	std::vector<IndustrySnapshot> industry_snapshots;     ///< Snapshots of industries indexed by IndustryID.
	std::vector<MilitaryContact> contacts;                ///< Known enemy contacts.
	uint32_t never_scouted_count = 0;                     ///< Count of never-scouted tiles.

	void Allocate(size_t map_size);
	void Reset();
};

/**
 * Authoritative visibility and intelligence subsystem.
 */
class MilitaryFog {
public:
	static bool IsActive();
	static void SetActive(bool active);

	static void Initialize();
	static void Reset();

	static uint8_t GetCompanyIntelligenceSide(CompanyID company);
	static void SetCompanyIntelligenceSide(CompanyID company, uint8_t side);
	static uint8_t GetLocalIntelligenceSide();

	/* Authoritative visibility queries */
	static FogStatus GetTileFogStatus(uint8_t side, TileIndex tile);
	static bool IsTileCurrentlyVisible(uint8_t side, TileIndex tile);
	static bool IsTileScouted(uint8_t side, TileIndex tile);

	static FogStatus GetTileFogStatusForLocal(TileIndex tile);
	static bool IsTileCurrentlyVisibleForLocal(TileIndex tile);

	/* Vehicle visibility rules */
	static bool ShouldHideVehicle(uint8_t side, const Vehicle *v);
	static bool ShouldHideVehicleForLocal(const Vehicle *v);

	/* Scouting and observation */
	static void UpdateAircraftScouting(DisasterVehicle *aircraft);
	static void UpdateAllVisibility();
	static void RefreshStationarySnapshot(uint8_t side, TileIndex tile);

	/* Targeting and exploration */
	static const MilitaryContact *FindNearestEligibleEnemyContact(const DisasterVehicle *aircraft);
	static TileIndex FindNearestNeverScoutedTile(const DisasterVehicle *aircraft);
	static bool CheckArrivalAndClearEmptyContact(DisasterVehicle *aircraft);

	/* UI snapshot inspection access */
	static SideIntelligence &GetSideIntelligence(uint8_t side);
	static const TownSnapshot *GetTownSnapshot(uint8_t side, TownID town);
	static const IndustrySnapshot *GetIndustrySnapshot(uint8_t side, IndustryID industry);

	/* Save / Load */
	static void Save();
	static void Load();
	static void ReconstructVisibility(bool refresh_unseen = false);
};

/**
 * RAII helper to swap active tile map buffers to a side's snapshot during UI rendering/inspection.
 */
class ActiveSnapshotScope {
private:
	Tile::TileBase *prev_base = nullptr;
	Tile::TileExtended *prev_extended = nullptr;
	bool active = false;

public:
	ActiveSnapshotScope(uint8_t side);
	~ActiveSnapshotScope();
};

#endif /* MILITARY_FOG_H */
