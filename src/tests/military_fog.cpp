/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file military_fog.cpp Comprehensive unit tests for fog of war and knowledge-limited military scouting. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../military_fog.h"
#include "../disaster_vehicle.h"
#include "../roadveh.h"
#include "../town.h"
#include "../industry.h"
#include "../map_func.h"
#include "../clear_map.h"
#include "../fileio_func.h"
#include "../gfx_func.h"
#include "../gfx_type.h"
#include "../table/sprites.h"
#include "../company_manager_face.h"
#include "../network/network.h"
#include "../saveload/saveload_func.h"
#include "mock_environment.h"
#include <cstdio>

#include "../safeguards.h"

static void ResetFogTestEnvironment()
{
	MockEnvironment::Instance();
	ResetFaces();
	_networking = true;
	_network_server = false;
	_network_dedicated = true;
	if (_cursor.sprites.empty()) {
		_cursor.sprites.emplace_back(SPR_CURSOR_MOUSE, PAL_NONE, 0, 0);
	}
	_searchpaths[Searchpath::WorkingDir] = "./";
	if (_valid_searchpaths.empty()) {
		_valid_searchpaths.push_back(Searchpath::WorkingDir);
	}
	_vehicle_pool.CleanPool();
	Map::Allocate(256, 256);
	for (TileIndex t = TileIndex{0}; t < TileIndex{Map::Size()}; t++) {
		MakeClear(t, ClearGround::Grass, 0);
		SetTileHeight(t, 0);
	}
	MilitaryFog::Reset();
	MilitaryFog::Initialize();
}

TEST_CASE("MilitaryFog - Initial fog of war and known geography snapshot")
{
	ResetFogTestEnvironment();

	REQUIRE(MilitaryFog::IsActive());
	uint8_t side0 = 0;

	/* Whole map starts unexplored (FOG_NEVER_SCOUTED) */
	TileIndex sample_tile = TileXY(100, 100);
	CHECK(MilitaryFog::GetTileFogStatus(side0, sample_tile) == FOG_NEVER_SCOUTED);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, sample_tile));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, sample_tile));

	const SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side0);
	CHECK(intel.never_scouted_count == Map::Size());

	/* Initial stationary snapshot matches starting world geography */
	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetTileType(sample_tile) == TileType::Clear);
		CHECK(TileHeight(sample_tile) == 0);
	}
}

TEST_CASE("MilitaryFog - Scouting radius and visibility state machine transitions")
{
	ResetFogTestEnvironment();
	uint8_t side = 0;

	TileIndex center_tile = TileXY(100, 100);
	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, side);
	REQUIRE(plane != nullptr);

	/* Tick aircraft: reveals scouting radius (MILITARY_SCOUT_RADIUS = 5) */
	plane->Tick();

	CHECK(MilitaryFog::GetTileFogStatus(side, center_tile) == FOG_CURRENTLY_VISIBLE);
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side, center_tile));
	CHECK(MilitaryFog::IsTileScouted(side, center_tile));

	/* Tile within radius 5 (e.g. 104, 100) is visible */
	TileIndex near_tile = TileXY(104, 100);
	CHECK(MilitaryFog::GetTileFogStatus(side, near_tile) == FOG_CURRENTLY_VISIBLE);

	/* Tile outside radius 5 (e.g. 110, 100) remains never scouted */
	TileIndex far_tile = TileXY(110, 100);
	CHECK(MilitaryFog::GetTileFogStatus(side, far_tile) == FOG_NEVER_SCOUTED);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side, far_tile));

	/* Move aircraft far away to (200, 200) and update visibility */
	plane->UpdatePosition(200 * TILE_SIZE, 200 * TILE_SIZE, GetAircraftFlightLevel(plane));
	MilitaryFog::UpdateAllVisibility();

	/* Original center tile transitions from currently visible to previously scouted */
	CHECK(MilitaryFog::GetTileFogStatus(side, center_tile) == FOG_PREVIOUSLY_SCOUTED);
	CHECK(MilitaryFog::IsTileScouted(side, center_tile));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side, center_tile));

	/* New center tile is now currently visible */
	TileIndex new_center = TileXY(200, 200);
	CHECK(MilitaryFog::GetTileFogStatus(side, new_center) == FOG_CURRENTLY_VISIBLE);
}

TEST_CASE("MilitaryFog - Stale additions (competitor infrastructure unobserved until scouted)")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	TileIndex target_tile = TileXY(150, 150);

	/* Initially Grass clear land */
	CHECK(GetClearGround(target_tile) == ClearGround::Grass);

	/* Competitor develops target_tile into Rough ground */
	MakeClear(target_tile, ClearGround::Rough, 0);
	CHECK(GetClearGround(target_tile) == ClearGround::Rough);

	/* For Side 0, target_tile is unseen: snapshot still shows original Grass */
	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetClearGround(target_tile) == ClearGround::Grass);
	}

	/* Side 0 aircraft scouts the tile */
	DisasterVehicle *scout = SpawnMilitaryAircraft(150 * TILE_SIZE, 150 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout != nullptr);
	scout->Tick();

	/* Once scouted, snapshot is refreshed to show Rough */
	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetClearGround(target_tile) == ClearGround::Rough);
	}
}

TEST_CASE("MilitaryFog - Stale destruction (removed structures linger until revisited)")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	TileIndex target_tile = TileXY(120, 120);

	/* Scout tile while it is Rocks */
	MakeClear(target_tile, ClearGround::Rocks, 0);
	CHECK(GetClearGround(target_tile) == ClearGround::Rocks);

	DisasterVehicle *scout = SpawnMilitaryAircraft(120 * TILE_SIZE, 120 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout != nullptr);
	scout->Tick();

	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetClearGround(target_tile) == ClearGround::Rocks);
	}

	/* Move scout far away */
	scout->UpdatePosition(10 * TILE_SIZE, 10 * TILE_SIZE, GetAircraftFlightLevel(scout));
	MilitaryFog::UpdateAllVisibility();

	/* Rocks is cleared to Grass on live map */
	MakeClear(target_tile, ClearGround::Grass, 0);
	CHECK(GetClearGround(target_tile) == ClearGround::Grass);

	/* While unscouted, snapshot still retains Rocks */
	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetClearGround(target_tile) == ClearGround::Rocks);
	}

	/* Move scout back and tick: snapshot refreshes to Grass */
	scout->UpdatePosition(120 * TILE_SIZE, 120 * TILE_SIZE, GetAircraftFlightLevel(scout));
	scout->Tick();

	{
		ActiveSnapshotScope scope(side0);
		CHECK(GetClearGround(target_tile) == ClearGround::Grass);
	}
}

TEST_CASE("MilitaryFog - Stale terrain (terraformed tiles linger at last-observed height)")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	TileIndex hill_tile = TileXY(80, 80);
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			SetTileHeight(TileXY(80 + dx, 80 + dy), 1);
		}
	}

	/* Scout hill tile */
	DisasterVehicle *scout = SpawnMilitaryAircraft(80 * TILE_SIZE, 80 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout != nullptr);
	scout->Tick();

	{
		ActiveSnapshotScope scope(side0);
		CHECK(TileHeight(hill_tile) == 1);
	}

	/* Scout moves away */
	scout->UpdatePosition(10 * TILE_SIZE, 10 * TILE_SIZE, GetAircraftFlightLevel(scout));
	MilitaryFog::UpdateAllVisibility();

	/* Live map terraforms patch to height 2 */
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			SetTileHeight(TileXY(80 + dx, 80 + dy), 2);
		}
	}
	CHECK(TileHeight(hill_tile) == 2);

	/* Unscouted snapshot still retains height 1 */
	{
		ActiveSnapshotScope scope(side0);
		CHECK(TileHeight(hill_tile) == 1);
	}

	/* Return and re-scout */
	scout->UpdatePosition(80 * TILE_SIZE, 80 * TILE_SIZE, GetAircraftFlightLevel(scout));
	scout->Tick();

	{
		ActiveSnapshotScope scope(side0);
		CHECK(TileHeight(hill_tile) == 2);
	}
}

TEST_CASE("MilitaryFog - Stale towns and industries refresh only upon re-scouting")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	/* Set up a town */
	REQUIRE(Town::CanAllocateItem());
	Town *town = Town::Create(TileXY(60, 60));
	REQUIRE(town != nullptr);
	town->cache.population = 500;
	town->cache.num_houses = 50;

	/* Initial snapshot captures initial town population */
	MilitaryFog::Initialize();
	const TownSnapshot *ts_initial = MilitaryFog::GetTownSnapshot(side0, town->index);
	REQUIRE(ts_initial != nullptr);
	CHECK(ts_initial->population == 500);

	/* Town grows in live simulation */
	town->cache.population = 1200;
	town->cache.num_houses = 120;

	/* Outside visibility, snapshot remains 500 */
	const TownSnapshot *ts_stale = MilitaryFog::GetTownSnapshot(side0, town->index);
	REQUIRE(ts_stale != nullptr);
	CHECK(ts_stale->population == 500);

	/* Scout flies over town */
	DisasterVehicle *scout = SpawnMilitaryAircraft(60 * TILE_SIZE, 60 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout != nullptr);
	scout->Tick();

	/* Snapshot refreshed to 1200 */
	const TownSnapshot *ts_refreshed = MilitaryFog::GetTownSnapshot(side0, town->index);
	REQUIRE(ts_refreshed != nullptr);
	CHECK(ts_refreshed->population == 1200);

	delete town;
}

TEST_CASE("MilitaryFog - Hidden military units and competitor vehicles without ghost sprites")
{
	ResetFogTestEnvironment();
	uint8_t my_side = 0;
	uint8_t enemy_side = 1;

	/* Place enemy aircraft and enemy marker at (140, 100) (unscouted) */
	DisasterVehicle *enemy_plane = SpawnMilitaryAircraft(140 * TILE_SIZE, 100 * TILE_SIZE, Direction::W, enemy_side);
	DisasterVehicle *enemy_marker = SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, enemy_side);
	REQUIRE(enemy_plane != nullptr);
	REQUIRE(enemy_marker != nullptr);

	/* Friendly aircraft at (100, 100) */
	DisasterVehicle *friendly_plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, my_side);
	REQUIRE(friendly_plane != nullptr);

	/* Outside scouting radius: enemy units are completely hidden */
	CHECK(MilitaryFog::ShouldHideVehicle(my_side, enemy_plane));
	CHECK(MilitaryFog::ShouldHideVehicle(my_side, enemy_marker));

	/* Friendly military units are never hidden */
	CHECK_FALSE(MilitaryFog::ShouldHideVehicle(my_side, friendly_plane));

	/* Move friendly aircraft within scouting radius */
	friendly_plane->UpdatePosition(138 * TILE_SIZE, 100 * TILE_SIZE, GetAircraftFlightLevel(friendly_plane));
	friendly_plane->Tick();

	/* Enemy units now visible */
	CHECK_FALSE(MilitaryFog::ShouldHideVehicle(my_side, enemy_plane));
	CHECK_FALSE(MilitaryFog::ShouldHideVehicle(my_side, enemy_marker));
}

TEST_CASE("MilitaryFog - Exploration fallback and safe idling when fully explored")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	/* Map has only friendly units. Aircraft has no known enemy contacts. */
	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, side0);
	REQUIRE(plane != nullptr);

	/* Tick: aircraft reveals starting radius, executes exploration behavior towards nearest never-scouted tile */
	plane->Tick();
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);
	CHECK(plane->big_ufo_destroyer_target == VehicleID::Invalid());
	CHECK(plane->dest_tile != INVALID_TILE);

	/* Simulate map fully explored */
	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side0);
	std::fill(intel.is_scouted.begin(), intel.is_scouted.end(), 1);
	intel.never_scouted_count = 0;

	/* Next tick: aircraft idles safely */
	plane->Tick();
	CHECK(plane->GetMilitaryMovementState() == MMS_IDLE);
	CHECK(plane->big_ufo_destroyer_target == VehicleID::Invalid());
}

TEST_CASE("MilitaryFog - Lost contact seeking and empty clearance upon arrival")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;
	uint8_t enemy_side = 1;

	/* Place enemy marker at (103, 100) (within scouting radius of (100, 100)) */
	DisasterVehicle *enemy = SpawnMilitaryTargetMarker(103 * TILE_SIZE, 100 * TILE_SIZE, enemy_side);
	REQUIRE(enemy != nullptr);

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, side0);
	REQUIRE(plane != nullptr);

	/* First tick: scouts enemy and establishes contact */
	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == enemy->index);

	/* Now simulate enemy moving or being destroyed (lost contact) */
	delete enemy;

	/* Aircraft advances towards contact's last known position */
	for (int i = 0; i < 60; i++) {
		plane->Tick();
		if (plane->big_ufo_destroyer_target == VehicleID::Invalid()) break;
	}

	/* Upon reaching empty location, contact is cleared and aircraft transitions to idle/exploration */
	CHECK(plane->big_ufo_destroyer_target == VehicleID::Invalid());
	const auto &contacts = MilitaryFog::GetSideIntelligence(side0).contacts;
	CHECK(contacts.empty());
}

TEST_CASE("MilitaryFog - Save/load roundtrip preserving snapshots, exploration, and contacts")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	/* Scout a region */
	DisasterVehicle *plane = SpawnMilitaryAircraft(50 * TILE_SIZE, 50 * TILE_SIZE, Direction::E, side0);
	REQUIRE(plane != nullptr);
	plane->Tick();

	TileIndex scouted_tile = TileXY(50, 50);
	CHECK(MilitaryFog::IsTileScouted(side0, scouted_tile));
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, scouted_tile));

	/* Add a contact */
	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side0);
	MilitaryContact c;
	c.vehicle_id = VehicleID{99};
	c.last_x = 55 * TILE_SIZE;
	c.last_y = 50 * TILE_SIZE;
	c.last_tile = TileXY(55, 50);
	c.side = 1;
	c.currently_visible = false;
	intel.contacts.push_back(c);

	/* Save game to temporary file */
	SaveLoadResult save_res = SaveOrLoad("test_fog.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	/* Reset fog state */
	MilitaryFog::Reset();
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, scouted_tile));

	/* Load game back from file */
	SaveLoadResult load_res = SaveOrLoad("test_fog.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_fog.sav");

	/* Verify state restored */
	REQUIRE(MilitaryFog::IsActive());
	CHECK(MilitaryFog::IsTileScouted(side0, scouted_tile));
	const auto &restored_contacts = MilitaryFog::GetSideIntelligence(side0).contacts;
	REQUIRE(restored_contacts.size() == 1);
	CHECK(restored_contacts[0].vehicle_id == VehicleID{99});
	CHECK(restored_contacts[0].last_tile == TileXY(55, 50));
}
