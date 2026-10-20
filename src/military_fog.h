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
#include "station_type.h"
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
	uint32_t handle = 0;                     ///< Unique side-local stationary town handle.
	TownID original_id = TownID::Invalid();   ///< Town ID when observed.
	TileIndex xy = INVALID_TILE;             ///< Town center tile.
	uint32_t population = 0;                 ///< Last-observed population of the town.
	uint32_t num_houses = 0;                 ///< Last-observed number of houses in the town.
	std::string name{};                      ///< Last-observed town name.
	bool larger_town = false;                ///< Whether town is a city.
	bool exists = false;                     ///< Whether the town existed when last observed.
	ViewportSign sign{};                     ///< Viewport sign coordinates and dimensions.

	/**
	 * Recompute and update viewport sign coordinates and dimensions for this town snapshot.
	 */
	void UpdateVirtCoord();

	/**
	 * Get the viewport or minimap label string for this town snapshot.
	 * @param small Whether to return the concise minimap label (true) or full main-map label (false).
	 * @return Formatted label string.
	 */
	std::string GetViewportString(bool small) const;
};

/** Stored stationary snapshot of a station or waypoint. */
struct StationSnapshot {
	uint32_t handle = 0;                          ///< Unique side-local stationary snapshot handle.
	uint32_t instance_id = 0;                     ///< Observed station instance lifetime identifier.
	StationID original_id = StationID::Invalid();  ///< Station ID when observed.
	bool is_waypoint = false;                     ///< True if waypoint, false if station.
	bool is_oilrig = false;                       ///< True if oil rig station (offset sign).
	TileIndex xy = INVALID_TILE;                  ///< Station sign tile location.
	std::string name{};                           ///< Last-observed station or waypoint name.
	StationFacilities facilities{};               ///< Last-observed station facilities.
	Owner owner = INVALID_OWNER;                  ///< Station owner company.
	Colours colour = Colours::Grey;               ///< Last-observed primary company colour.
	bool exists = false;                          ///< Whether the station existed when last observed.
	ViewportSign sign{};                          ///< Viewport sign coordinates and dimensions.
	std::vector<TileIndex> observed_tiles{};      ///< Observed footprint tiles belonging to this snapshot.
	std::vector<StationFacilities> tile_facilities{}; ///< Remembered facility contributions per observed footprint tile.

	/**
	 * Record or update the facility contribution of an observed tile.
	 * @param tile Tile to set contribution for.
	 * @param fac Facilities contributed by this tile.
	 */
	void SetTileContribution(TileIndex tile, StationFacilities fac);

	/**
	 * Remove the facility contribution of an observed tile.
	 * @param tile Tile to remove contribution for.
	 */
	void RemoveTileContribution(TileIndex tile);

	/**
	 * Recompute aggregate snapshot facilities and oilrig status from remembered tile contributions.
	 */
	void RecomputeAggregateFacilities();

	/**
	 * Check whether this snapshot represents an active station or waypoint with transport facilities.
	 * @return true if remembered transport facilities are present.
	 */
	inline bool IsInUse() const
	{
		return this->facilities.Any({StationFacility::Train, StationFacility::TruckStop, StationFacility::BusStop, StationFacility::Airport, StationFacility::Dock});
	}

	/**
	 * Get the display sign colour for this station snapshot.
	 * @return Observed colour if owner is valid and station is in use, Colours::Grey otherwise.
	 */
	inline Colours GetDisplayColour() const
	{
		return (this->owner == OWNER_NONE || !this->IsInUse()) ? Colours::Grey : this->colour;
	}

	/**
	 * Recompute and update viewport sign coordinates and dimensions for this station snapshot.
	 */
	void UpdateVirtCoord();

	/**
	 * Get the viewport label string for this station snapshot.
	 * @param small Whether to return the concise label (true) or full label with facility icons (false).
	 * @return Formatted label string.
	 */
	std::string GetViewportString(bool small) const;
};

/** Stored stationary snapshot of an industry. */
struct IndustrySnapshot {
	uint32_t handle = 0;               ///< Unique side-local stationary snapshot handle.
	IndustryType type = 0;              ///< Industry type ID.
	TileIndex location = INVALID_TILE; ///< Industry location center/origin tile.
	uint8_t prod_level = 0;            ///< Last-observed production level.
	Owner owner = OWNER_NONE;          ///< Owner of the industry.
	Colours colour = Colours::Grey;    ///< Industry colour for map drawing.
	std::string name{};                ///< Last-observed industry name.
	bool exists = false;               ///< Whether the industry existed when last observed.
	std::vector<TileIndex> observed_tiles{}; ///< Observed footprint tiles belonging to this snapshot.
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
	std::vector<uint32_t> tile_to_industry_handle;        ///< Map from TileIndex.base() to snapshot handle (0 = none).
	std::vector<uint32_t> tile_to_station_handle;         ///< Map from TileIndex.base() to station snapshot handle (0 = none).
	std::unique_ptr<Tile::TileBase[]> base_tiles;         ///< Stationary snapshot of TileBase data.
	std::unique_ptr<Tile::TileExtended[]> extended_tiles; ///< Stationary snapshot of TileExtended data.
	std::vector<TownSnapshot> town_snapshots;             ///< Snapshots of towns indexed by TownID.
	std::vector<StationSnapshot> station_snapshots;       ///< Snapshots of stations and waypoints known to this side.
	std::vector<IndustrySnapshot> industry_snapshots;     ///< Snapshots of industries known to this side.
	std::vector<MilitaryContact> contacts;                ///< Known enemy contacts.
	uint32_t never_scouted_count = 0;                     ///< Count of never-scouted tiles.
	uint32_t next_industry_handle = 1;                    ///< Next stationary handle to allocate.
	uint32_t next_town_handle = 1;                        ///< Next stationary town handle to allocate.
	uint32_t next_station_handle = 1;                     ///< Next stationary station handle to allocate.

	/**
	 * Allocate memory structures for a given map size.
	 * @param map_size Total number of tiles on the map.
	 */
	void Allocate(size_t map_size);

	/**
	 * Reset and deallocate intelligence state.
	 */
	void Reset();
};

/**
 * Authoritative visibility and intelligence subsystem.
 */
class MilitaryFog {
public:
	/**
	 * Check whether military fog of war is currently enabled.
	 * @return True if fog of war is active, false otherwise.
	 */
	static bool IsActive();

	/**
	 * Set military fog of war active status.
	 * @param active True to activate, false to deactivate.
	 */
	static void SetActive(bool active);

	/**
	 * Initialize military fog of war state for all sides on the active map.
	 */
	static void Initialize();

	/**
	 * Reset and clear all military fog of war state and deallocate snapshot buffers.
	 */
	static void Reset();

	/**
	 * Get the intelligence side affiliation for a transport company.
	 * @param company The company ID to query.
	 * @return Side index (0 to MAX_MILITARY_SIDES - 1).
	 */
	static uint8_t GetCompanyIntelligenceSide(CompanyID company);

	/**
	 * Set the intelligence side affiliation for a transport company.
	 * @param company The company ID to configure.
	 * @param side Side index (0 to MAX_MILITARY_SIDES - 1).
	 */
	static void SetCompanyIntelligenceSide(CompanyID company, uint8_t side);

	/**
	 * Get the intelligence side affiliation of the local client company.
	 * @return Side index of local company.
	 */
	static uint8_t GetLocalIntelligenceSide();

	/* Authoritative visibility queries */

	/**
	 * Query fog of war visibility status of a tile for a specific side.
	 * @param side Side index.
	 * @param tile Tile to query.
	 * @return FogStatus of the specified tile.
	 */
	static FogStatus GetTileFogStatus(uint8_t side, TileIndex tile);

	/**
	 * Check if a tile is currently in active scouting radius of a side's units.
	 * @param side Side index.
	 * @param tile Tile to query.
	 * @return True if currently visible, false otherwise.
	 */
	static bool IsTileCurrentlyVisible(uint8_t side, TileIndex tile);

	/**
	 * Check if a tile has ever been scouted by a side.
	 * @param side Side index.
	 * @param tile Tile to query.
	 * @return True if previously or currently scouted, false if never scouted.
	 */
	static bool IsTileScouted(uint8_t side, TileIndex tile);

	/**
	 * Query fog of war visibility status of a tile for the local company.
	 * @param tile Tile to query.
	 * @return FogStatus for the local company's side.
	 */
	static FogStatus GetTileFogStatusForLocal(TileIndex tile);

	/**
	 * Check if a tile is currently visible to the local company.
	 * @param tile Tile to query.
	 * @return True if currently visible, false otherwise.
	 */
	static bool IsTileCurrentlyVisibleForLocal(TileIndex tile);

	/* Vehicle visibility rules */

	/**
	 * Check if a vehicle should be hidden behind fog for a specific side.
	 * @param side Side index observing.
	 * @param v Vehicle to evaluate.
	 * @return True if vehicle should be hidden, false if visible.
	 */
	static bool ShouldHideVehicle(uint8_t side, const Vehicle *v);

	/**
	 * Check if a vehicle should be hidden behind fog for the local client.
	 * @param v Vehicle to evaluate.
	 * @return True if vehicle should be hidden, false if visible.
	 */
	static bool ShouldHideVehicleForLocal(const Vehicle *v);

	/* Scouting and observation */

	/**
	 * Update tile visibility and contacts scouted by a specific military aircraft.
	 * @param aircraft Aircraft performing scouting.
	 */
	static void UpdateAircraftScouting(DisasterVehicle *aircraft);

	/**
	 * Recalculate dynamic tile visibility across all military sides and active aircraft.
	 */
	static void UpdateAllVisibility();

	/**
	 * Refresh stationary snapshot data for a specific tile.
	 * @param side Side index.
	 * @param tile Tile to refresh snapshot for.
	 */
	static void RefreshStationarySnapshot(uint8_t side, TileIndex tile);

	/* Targeting and exploration */

	/**
	 * Find nearest known enemy contact for an aircraft from remembered contacts.
	 * @param aircraft Searching aircraft.
	 * @return Pointer to nearest contact, or nullptr if none known.
	 */
	static const MilitaryContact *FindNearestEligibleEnemyContact(const DisasterVehicle *aircraft);

	/**
	 * Find nearest never-scouted tile on the map for autonomous exploration.
	 * @param aircraft Searching aircraft.
	 * @return TileIndex of nearest never-scouted tile, or INVALID_TILE if map fully scouted.
	 */
	static TileIndex FindNearestNeverScoutedTile(const DisasterVehicle *aircraft);

	/**
	 * Check arrival at a target contact location and remove contact if target is absent.
	 * @param aircraft Arriving aircraft.
	 * @return True if contact was cleared, false otherwise.
	 */
	static bool CheckArrivalAndClearEmptyContact(DisasterVehicle *aircraft);

	/* UI snapshot inspection access */

	/**
	 * Access side intelligence state structure.
	 * @param side Side index.
	 * @return Reference to SideIntelligence structure.
	 */
	static SideIntelligence &GetSideIntelligence(uint8_t side);

	/**
	 * Get stored snapshot of a town for a specific side.
	 * @param side Side index.
	 * @param town Town ID.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const TownSnapshot *GetTownSnapshot(uint8_t side, TownID town);

	/**
	 * Get stored snapshot of a town for a specific side by tile location.
	 * @param side Side index.
	 * @param tile Tile to query.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const TownSnapshot *GetTownSnapshotAtTile(uint8_t side, TileIndex tile);

	/**
	 * Update sign coordinates and dimensions for all town snapshots across all sides.
	 */
	static void UpdateAllTownSnapshotVirtCoords();

	/**
	 * Get stored snapshot of a station or waypoint for a specific side by stationary handle.
	 * @param side Side index.
	 * @param handle Stationary station handle.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const StationSnapshot *GetStationSnapshotByHandle(uint8_t side, uint32_t handle);

	/**
	 * Get stored snapshot of a station or waypoint for a specific side by tile location.
	 * @param side Side index.
	 * @param tile Tile located within station footprint or at sign location.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const StationSnapshot *GetStationSnapshotAtTile(uint8_t side, TileIndex tile);

	/**
	 * Get stored snapshot of a station or waypoint for a specific side by original ID.
	 * @param side Side index.
	 * @param station Station ID.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const StationSnapshot *GetStationSnapshot(uint8_t side, StationID station);

	/**
	 * Get stored snapshot of a station or waypoint for a specific side by instance ID.
	 * @param side Side index.
	 * @param instance_id Station instance ID.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const StationSnapshot *GetStationSnapshotByInstance(uint8_t side, uint32_t instance_id);

	/**
	 * Reset the station instance counter (for tests).
	 */
	static void ResetStationInstanceCounter();

	/**
	 * Get stored snapshot of an industry for a specific side by tile location.
	 * @param side Side index.
	 * @param tile Tile to query.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const IndustrySnapshot *GetIndustrySnapshotAtTile(uint8_t side, TileIndex tile);

	/**
	 * Get stored snapshot of an industry for a specific side by stationary handle.
	 * @param side Side index.
	 * @param handle Stationary industry handle.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const IndustrySnapshot *GetIndustrySnapshotByHandle(uint8_t side, uint32_t handle);

	/**
	 * Get stored snapshot of an industry for a specific side by root/origin location.
	 * @param side Side index.
	 * @param location Industry root tile location.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const IndustrySnapshot *GetIndustrySnapshotAtLocation(uint8_t side, TileIndex location);

	/**
	 * Get stored snapshot of an industry for a specific side.
	 * @param side Side index.
	 * @param industry Industry ID.
	 * @return Pointer to snapshot, or nullptr if not available.
	 */
	static const IndustrySnapshot *GetIndustrySnapshot(uint8_t side, IndustryID industry);

	/**
	 * Get the remembered count of industries of a specific type for a side.
	 * @param side Side index.
	 * @param type Industry type ID.
	 * @return Count of remembered industries of the given type.
	 */
	static size_t GetIndustryTypeCount(uint8_t side, IndustryType type);

	/**
	 * Get the remembered production level of an industry at root location.
	 * @param side Side index.
	 * @param location Root tile of the industry.
	 * @return Remembered production level, or 0 if unknown.
	 */
	static uint8_t GetIndustryProduction(uint8_t side, TileIndex location);

	/* Save / Load */

	/**
	 * Save military fog chunk data.
	 */
	static void Save();

	/**
	 * Load military fog chunk data.
	 */
	static void Load();

	/**
	 * Reconstruct tile visibility post-load.
	 * @param refresh_unseen Optional flag to refresh unseen snapshots.
	 */
	static void ReconstructVisibility(bool refresh_unseen = false);
};

/**
 * RAII helper to swap active tile map buffers to a side's snapshot during UI rendering/inspection.
 */
class ActiveSnapshotScope {
private:
	Tile::TileBase *prev_base = nullptr;         ///< Original base tile pointer.
	Tile::TileExtended *prev_extended = nullptr; ///< Original extended tile pointer.
	uint8_t prev_side = 0xFF;                    ///< Previous active side index.
	bool active = false;                         ///< Whether buffer swap is currently active.

	inline static uint8_t _active_side = 0xFF;   ///< Currently active side for snapshot scope.

public:
	/**
	 * Construct scope and swap active map buffers to the given side's snapshot.
	 * @param side Side index to inspect.
	 */
	ActiveSnapshotScope(uint8_t side);

	/**
	 * Destruct scope and restore active map buffers.
	 */
	~ActiveSnapshotScope();

	/**
	 * Check whether snapshot scope is currently active.
	 * @return true if snapshot buffer swap is active.
	 */
	static bool IsActive() { return _active_side != 0xFF; }

	/**
	 * Get the currently active snapshot side.
	 * @return Active side index.
	 */
	static uint8_t GetCurrentSide() { return _active_side; }
};

#endif /* MILITARY_FOG_H */
