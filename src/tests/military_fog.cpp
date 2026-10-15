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
#include "../industry_map.h"
#include "../map_func.h"
#include "../clear_map.h"
#include "../fileio_func.h"
#include "../gfx_func.h"
#include "../gfx_type.h"
#include "../viewport_func.h"
#include "../table/sprites.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../company_manager_face.h"
#include "../object_base.h"

#include "../network/network.h"
#include "../saveload/saveload_func.h"
#include "../tile_cmd.h"
#include "../void_map.h"
#include "../vehicle_func.h"
#include "mock_environment.h"
#include <cstdio>
#include <filesystem>

#include "../safeguards.h"

extern void InitializeGame(uint size_x, uint size_y, bool reset_date, bool reset_settings);

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
	if (std::filesystem::exists("lang")) {
		_searchpaths[Searchpath::WorkingDir] = "./";
	} else if (std::filesystem::exists("build/lang")) {
		_searchpaths[Searchpath::WorkingDir] = "build/";
	} else {
		_searchpaths[Searchpath::WorkingDir] = "./";
	}
	if (_valid_searchpaths.empty()) {
		_valid_searchpaths.push_back(Searchpath::WorkingDir);
	}
	_vehicle_pool.CleanPool();
	_town_pool.CleanPool();
	_industry_pool.CleanPool();
	_object_pool.CleanPool();
	ResetIndustries();
	_settings_game.game_creation.map_x = 8;
	_settings_game.game_creation.map_y = 8;
	Map::Allocate(256, 256);
	for (TileIndex t = TileIndex{0}; t < TileIndex{Map::Size()}; t++) {
		if (IsInnerTile(t)) {
			MakeClear(t, ClearGround::Grass, 0);
		} else {
			MakeVoid(t);
		}
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

	/* Test transport company vehicle visibility rules:
	 * Competitor transport company vehicles behind fog must be hidden even if affiliated with the same side.
	 * Local company's own transport vehicles must always remain visible.
	 */
	_company_pool.CleanPool();
	REQUIRE(Company::CanAllocateItem(2));
	Company *c1 = Company::CreateAtIndex(CompanyID{0});
	Company *c2 = Company::CreateAtIndex(CompanyID{1});
	REQUIRE(c1 != nullptr);
	REQUIRE(c2 != nullptr);

	_local_company = CompanyID{0};
	CompanyID comp1 = CompanyID{0}; // Local company
	CompanyID comp2 = CompanyID{1}; // Competitor company

	MilitaryFog::SetCompanyIntelligenceSide(comp1, my_side);
	MilitaryFog::SetCompanyIntelligenceSide(comp2, my_side); // Same side!

	REQUIRE(Vehicle::CanAllocateItem());
	RoadVehicle *my_veh = RoadVehicle::Create();
	REQUIRE(my_veh != nullptr);
	my_veh->owner = comp1;
	my_veh->tile = TileXY(180, 180);
	my_veh->x_pos = 180 * TILE_SIZE;
	my_veh->y_pos = 180 * TILE_SIZE;

	REQUIRE(Vehicle::CanAllocateItem());
	RoadVehicle *comp_veh = RoadVehicle::Create();
	REQUIRE(comp_veh != nullptr);
	comp_veh->owner = comp2;
	comp_veh->tile = TileXY(180, 180);
	comp_veh->x_pos = 180 * TILE_SIZE;
	comp_veh->y_pos = 180 * TILE_SIZE;

	/* Tile (180, 180) is not currently visible */
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(my_side, TileXY(180, 180)));

	/* Own vehicle is never hidden */
	CHECK_FALSE(MilitaryFog::ShouldHideVehicle(my_side, my_veh));
	CHECK_FALSE(MilitaryFog::ShouldHideVehicleForLocal(my_veh));

	/* Competitor vehicle IS hidden even though assigned to the same intelligence side */
	CHECK(MilitaryFog::ShouldHideVehicle(my_side, comp_veh));
	CHECK(MilitaryFog::ShouldHideVehicleForLocal(comp_veh));

	delete my_veh;
	delete comp_veh;
	_company_pool.CleanPool();
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

TEST_CASE("MilitaryFog - Two-side intelligence isolation")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;
	uint8_t side1 = 1;

	/* Side 0 aircraft scouts tile (80, 80) */
	TileIndex scout_tile = TileXY(80, 80);
	DisasterVehicle *plane0 = SpawnMilitaryAircraft(80 * TILE_SIZE, 80 * TILE_SIZE, Direction::E, side0);
	REQUIRE(plane0 != nullptr);
	plane0->Tick();

	/* Side 0 knows the tile */
	CHECK(MilitaryFog::IsTileScouted(side0, scout_tile));
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, scout_tile));

	/* Side 1 must remain in total fog of war (isolated) */
	CHECK_FALSE(MilitaryFog::IsTileScouted(side1, scout_tile));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side1, scout_tile));
	CHECK(MilitaryFog::GetTileFogStatus(side1, scout_tile) == FOG_NEVER_SCOUTED);
}

TEST_CASE("MilitaryFog - Hidden target movement or destruction does not update retained contact")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;
	uint8_t side1 = 1;

	/* Place enemy aircraft at (104, 100) and scout it */
	DisasterVehicle *enemy = SpawnMilitaryAircraft(104 * TILE_SIZE, 100 * TILE_SIZE, Direction::W, side1);
	REQUIRE(enemy != nullptr);

	DisasterVehicle *scout = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout != nullptr);
	scout->Tick();

	/* Side 0 establishes contact at (104, 100) */
	const auto &intel = MilitaryFog::GetSideIntelligence(side0);
	REQUIRE(intel.contacts.size() == 1);
	CHECK(intel.contacts[0].vehicle_id == enemy->index);
	CHECK(intel.contacts[0].last_tile == TileXY(104, 100));

	/* Scout moves far away, losing visibility */
	scout->UpdatePosition(10 * TILE_SIZE, 10 * TILE_SIZE, GetAircraftFlightLevel(scout));
	MilitaryFog::UpdateAllVisibility();
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(104, 100)));

	/* While hidden in fog, enemy moves to (130, 100) */
	enemy->UpdatePosition(130 * TILE_SIZE, 100 * TILE_SIZE, GetAircraftFlightLevel(enemy));

	/* Tick visibility / game tick without scout observing */
	MilitaryFog::UpdateAllVisibility();

	/* Retained contact MUST NOT be updated to new hidden coordinates */
	const auto &contacts_after = MilitaryFog::GetSideIntelligence(side0).contacts;
	REQUIRE(contacts_after.size() == 1);
	CHECK(contacts_after[0].last_tile == TileXY(104, 100));
	CHECK_FALSE(contacts_after[0].currently_visible);

	/* Delete enemy while in fog */
	delete enemy;
	MilitaryFog::UpdateAllVisibility();

	/* Contact remains retained with last known position until re-observed */
	const auto &contacts_after_delete = MilitaryFog::GetSideIntelligence(side0).contacts;
	REQUIRE(contacts_after_delete.size() == 1);
	CHECK(contacts_after_delete[0].last_tile == TileXY(104, 100));
}

TEST_CASE("MilitaryFog - World lifecycle boundary and military-to-civilian transitions")
{
	ResetFogTestEnvironment();
	REQUIRE(MilitaryFog::IsActive());
	uint8_t side0 = 0;
	TileIndex t = TileXY(50, 50);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, t));

	/* Scout a tile so there is active knowledge */
	MilitaryFog::GetSideIntelligence(side0).is_scouted[t.base()] = 1;
	MilitaryFog::GetSideIntelligence(side0).never_scouted_count--;
	CHECK(MilitaryFog::IsTileScouted(side0, t));

	/* 1. Map-size transition starting with active fog */
	Map::Allocate(128, 128);
	CHECK_FALSE(MilitaryFog::IsActive());
	CHECK(MilitaryFog::GetSideIntelligence(side0).never_scouted_count == 0);
	CHECK(MilitaryFog::GetSideIntelligence(side0).is_scouted.empty());
	CHECK(MilitaryFog::GetSideIntelligence(side0).base_tiles == nullptr);

	/* 2. InitializeGame() resets fog and buffers */
	MilitaryFog::Initialize();
	REQUIRE(MilitaryFog::IsActive());
	InitializeGame(64, 64, true, true);
	CHECK_FALSE(MilitaryFog::IsActive());
	CHECK(MilitaryFog::GetSideIntelligence(side0).never_scouted_count == 0);
	CHECK(MilitaryFog::GetSideIntelligence(side0).is_scouted.empty());
	CHECK(MilitaryFog::GetSideIntelligence(side0).base_tiles == nullptr);

	/* 3. Loading a real pre-fog save where MFOG chunk is completely absent */
	/* Re-activate military fog and scout tiles on 256x256 before loading */
	ResetFogTestEnvironment();
	extern void ResetHouses();
	ResetHouses();
	extern void InitializeLanguagePacks();
	InitializeLanguagePacks();
	REQUIRE(MilitaryFog::IsActive());
	MilitaryFog::GetSideIntelligence(side0).is_scouted[t.base()] = 1;
	MilitaryFog::GetSideIntelligence(side0).never_scouted_count--;
	REQUIRE(MilitaryFog::IsTileScouted(side0, t));

	std::string sav_path = "regression/aircraft/test.sav";
	if (!std::filesystem::exists(sav_path)) sav_path = "game/aircraft/test.sav";
	if (!std::filesystem::exists(sav_path)) sav_path = "../regression/aircraft/test.sav";
	SaveLoadResult load_res = SaveOrLoad(sav_path, SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);

	/* Post-load: military fog must be inactive and buffers clean */
	CHECK_FALSE(MilitaryFog::IsActive());
	CHECK(MilitaryFog::GetSideIntelligence(side0).is_scouted.empty());
	CHECK(MilitaryFog::GetSideIntelligence(side0).never_scouted_count == 0);
	CHECK(MilitaryFog::GetSideIntelligence(side0).contacts.empty());
	CHECK(MilitaryFog::GetSideIntelligence(side0).base_tiles == nullptr);
}

TEST_CASE("MilitaryFog - Normal tick lifecycle CallVehicleTicks and ResetMilitaryScenario")
{
	ResetFogTestEnvironment();

	/* Setup the military fixture scenario */
	bool ok = SetupMilitaryScenario();
	REQUIRE(ok);
	REQUIRE(MilitaryFog::IsActive());

	uint8_t side0 = 0;
	DisasterVehicle *scout = nullptr;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT) {
			scout = v;
			break;
		}
	}
	REQUIRE(scout != nullptr);
	int start_x = scout->x_pos;
	int start_y = scout->y_pos;
	TileIndex start_tile = TileVirtXY(start_x, start_y);

	/* 1. Tick once: initial aircraft scout radius is computed and visible */
	CallVehicleTicks();
	CHECK(MilitaryFog::GetTileFogStatus(side0, start_tile) == FOG_CURRENTLY_VISIBLE);
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, start_tile));
	CHECK(MilitaryFog::IsTileScouted(side0, start_tile));

	/* 2. Run 120 normal game ticks using CallVehicleTicks() - no manual UpdateAllVisibility() */
	for (int i = 0; i < 120; i++) {
		CallVehicleTicks();
	}

	/* Plane moved autonomously (changed position by > 80 pixels) */
	CHECK((scout->x_pos != start_x || scout->y_pos != start_y));
	TileIndex current_tile = TileVirtXY(scout->x_pos, scout->y_pos);
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, current_tile));

	/* Moving out of previously visible area: start_tile transitioned to PREVIOUSLY_SCOUTED */
	CHECK(MilitaryFog::GetTileFogStatus(side0, start_tile) == FOG_PREVIOUSLY_SCOUTED);
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, start_tile));
	CHECK(MilitaryFog::IsTileScouted(side0, start_tile));

	/* 3. Reset scenario: disables fog and clears units/knowledge */
	ResetMilitaryScenario();
	CHECK_FALSE(MilitaryFog::IsActive());

	size_t remaining_military_units = 0;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT ||
				v->subtype == ST_MILITARY_AIRCRAFT_SHADOW ||
				v->subtype == ST_MILITARY_TARGET_MARKER) {
			remaining_military_units++;
		}
	}
	CHECK(remaining_military_units == 0);
	CHECK(MilitaryFog::GetSideIntelligence(side0).is_scouted.empty());
	CHECK(MilitaryFog::GetSideIntelligence(side0).contacts.empty());
}

TEST_CASE("MilitaryFog - Uninterrupted versus loaded normal-tick continuation with contact and isolation")
{
	uint8_t side0 = 0;
	uint8_t side1 = 1;

	auto setup_scenario = [&]() -> std::pair<DisasterVehicle *, DisasterVehicle *> {
		ResetFogTestEnvironment();

		/* Side 0 scout aircraft at (100, 100) facing East */
		DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, side0);

		/* Side 1 enemy marker at (104, 100) */
		DisasterVehicle *enemy = SpawnMilitaryTargetMarker(104 * TILE_SIZE, 100 * TILE_SIZE, side1);

		return {plane, enemy};
	};

	auto find_contact = [](const std::vector<MilitaryContact> &contacts, VehicleID vid) -> const MilitaryContact * {
		for (const auto &c : contacts) {
			if (c.vehicle_id == vid) return &c;
		}
		return nullptr;
	};

	/* --- RUN A: Uninterrupted simulation --- */
	auto [planeA, enemyA] = setup_scenario();
	REQUIRE(planeA != nullptr);
	REQUIRE(enemyA != nullptr);

	/* Tick 1: plane observes enemy at (104, 100) */
	CallVehicleTicks();
	REQUIRE(MilitaryFog::GetSideIntelligence(side0).contacts.size() == 1);
	CHECK(MilitaryFog::GetSideIntelligence(side0).contacts[0].currently_visible);
	CHECK(MilitaryFog::GetSideIntelligence(side0).contacts[0].vehicle_id == enemyA->index);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side1, TileXY(100, 100))); // Side 1 isolation

	/* Add wide contact with 32-bit VehicleID and coordinates > 65535 to detect truncation */
	MilitaryContact wide_contact;
	wide_contact.vehicle_id = VehicleID{0x70000};
	wide_contact.last_x = 180 * TILE_SIZE + 5;
	wide_contact.last_y = 180 * TILE_SIZE + 7;
	wide_contact.last_tile = TileXY(180, 180);
	wide_contact.side = side1;
	wide_contact.currently_visible = false;
	MilitaryFog::GetSideIntelligence(side0).contacts.push_back(wide_contact);

	/* Scout flies away to (10, 10) and normal tick runs: visibility of (104, 100) is lost */
	planeA->UpdatePosition(10 * TILE_SIZE, 10 * TILE_SIZE, GetAircraftFlightLevel(planeA));
	CallVehicleTicks();

	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(104, 100)));
	CHECK(MilitaryFog::IsTileScouted(side0, TileXY(104, 100)));

	const MilitaryContact *c_enemyA = find_contact(MilitaryFog::GetSideIntelligence(side0).contacts, enemyA->index);
	REQUIRE(c_enemyA != nullptr);
	CHECK_FALSE(c_enemyA->currently_visible);
	CHECK(c_enemyA->last_tile == TileXY(104, 100));

	/* Continue Run A for 20 more normal ticks */
	for (int i = 0; i < 20; i++) {
		CallVehicleTicks();
	}

	int uninterrupted_x = planeA->x_pos;
	int uninterrupted_y = planeA->y_pos;
	TileIndex uninterrupted_dest = planeA->dest_tile;
	uint32_t uninterrupted_scouted = MilitaryFog::GetSideIntelligence(side0).never_scouted_count;
	size_t uninterrupted_contact_count = MilitaryFog::GetSideIntelligence(side0).contacts.size();

	/* --- RUN B: Save at lost-visibility state, load, and continue for 20 normal ticks --- */
	auto [planeB, enemyB] = setup_scenario();
	REQUIRE(planeB != nullptr);
	REQUIRE(enemyB != nullptr);
	VehicleID enemy_id = enemyB->index;

	/* Tick 1: discover enemy */
	CallVehicleTicks();

	/* Add same wide contact */
	MilitaryFog::GetSideIntelligence(side0).contacts.push_back(wide_contact);

	/* Relocate to (10, 10) and tick: lose visibility */
	planeB->UpdatePosition(10 * TILE_SIZE, 10 * TILE_SIZE, GetAircraftFlightLevel(planeB));
	CallVehicleTicks();

	/* Save at this exact state */
	SaveLoadResult save_res = SaveOrLoad("continuation_contact.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	/* Reset environment */
	MilitaryFog::Reset();
	_vehicle_pool.CleanPool();

	/* Load back */
	SaveLoadResult load_res = SaveOrLoad("continuation_contact.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("continuation_contact.sav");

	/* Verify loaded state immediately post-load */
	REQUIRE(MilitaryFog::IsActive());
	const auto &loaded_intel0 = MilitaryFog::GetSideIntelligence(side0);
	REQUIRE(loaded_intel0.contacts.size() == 2);

	const MilitaryContact *loaded_c_enemy = find_contact(loaded_intel0.contacts, enemy_id);
	REQUIRE(loaded_c_enemy != nullptr);
	CHECK(loaded_c_enemy->last_tile == TileXY(104, 100));
	CHECK_FALSE(loaded_c_enemy->currently_visible);

	const MilitaryContact *loaded_c_wide = find_contact(loaded_intel0.contacts, VehicleID{0x70000});
	REQUIRE(loaded_c_wide != nullptr);
	CHECK(loaded_c_wide->last_x == 180 * TILE_SIZE + 5);
	CHECK(loaded_c_wide->last_y == 180 * TILE_SIZE + 7);
	CHECK(loaded_c_wide->last_tile == TileXY(180, 180));
	CHECK_FALSE(loaded_c_wide->currently_visible);

	/* Immediately post-load assertion of reconstructed current visibility before normal ticks run */
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(10, 10)));
	CHECK(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(10 + MILITARY_SCOUT_RADIUS / 2, 10)));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(104, 100)));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side0, TileXY(100, 100)));
	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(side1, TileXY(10, 10)));

	/* Side 1 isolation preserved */
	const auto &loaded_intel1 = MilitaryFog::GetSideIntelligence(side1);
	CHECK(loaded_intel1.contacts.empty());
	CHECK_FALSE(MilitaryFog::IsTileScouted(side1, TileXY(100, 100)));
	CHECK_FALSE(MilitaryFog::IsTileScouted(side1, TileXY(180, 180)));

	DisasterVehicle *loaded_plane = nullptr;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT) {
			loaded_plane = v;
			break;
		}
	}
	REQUIRE(loaded_plane != nullptr);

	/* Continue Run B for 20 normal ticks */
	for (int i = 0; i < 20; i++) {
		CallVehicleTicks();
	}

	/* Deterministic continuation match */
	CHECK(loaded_plane->x_pos == uninterrupted_x);
	CHECK(loaded_plane->y_pos == uninterrupted_y);
	CHECK(loaded_plane->dest_tile == uninterrupted_dest);
	CHECK(MilitaryFog::GetSideIntelligence(side0).never_scouted_count == uninterrupted_scouted);
	CHECK(MilitaryFog::GetSideIntelligence(side0).contacts.size() == uninterrupted_contact_count);
}

TEST_CASE("MilitaryFog - Contact count 32-bit boundary serialization")
{
	ResetFogTestEnvironment();
	Map::Allocate(4096, 64);
	MilitaryFog::Initialize();

	uint8_t side0 = 0;
	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side0);
	intel.contacts.clear();

	constexpr uint32_t NUM_CONTACTS = 65536;
	intel.contacts.reserve(NUM_CONTACTS);

	/* Add 65,536 contacts with 32-bit VehicleIDs, valid coordinates > 32,767 (signed 16-bit threshold),
	 * and valid tile indices >= 65,536 (16-bit unsigned threshold) on a supported 4096x64 map. */
	for (uint32_t i = 0; i < NUM_CONTACTS; i++) {
		MilitaryContact c;
		c.vehicle_id = VehicleID{0x10000 + i};
		/* Coordinates in [33,000, 64,999] are strictly > 32,767 (detecting signed 16-bit overflow)
		 * while within map X dimension (4096 * 16 = 65,536 units). */
		c.last_x = static_cast<int>(33000 + (i % 32000));
		c.last_y = static_cast<int>((i % 60) * 16 + 8);
		/* Tile indices with tile_y in [16, 61] have tile index in [65,536, 253,755],
		 * detecting 16-bit unsigned wrap within valid map size Map::Size(). */
		uint tile_x = 100 + (i % 3800);
		uint tile_y = 16 + ((i / 3800) % 46);
		c.last_tile = TileXY(tile_x, tile_y);
		c.side = 1;
		c.currently_visible = (i % 2 == 0);
		intel.contacts.push_back(c);
	}

	SaveLoadResult save_res = SaveOrLoad("test_contacts_boundary.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	MilitaryFog::Reset();
	REQUIRE(MilitaryFog::GetSideIntelligence(side0).contacts.empty());

	SaveLoadResult load_res = SaveOrLoad("test_contacts_boundary.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_contacts_boundary.sav");

	const auto &restored = MilitaryFog::GetSideIntelligence(side0).contacts;
	REQUIRE(restored.size() == NUM_CONTACTS);
	for (uint32_t i = 0; i < NUM_CONTACTS; i += 257) {
		uint tile_x = 100 + (i % 3800);
		uint tile_y = 16 + ((i / 3800) % 46);
		CHECK(restored[i].vehicle_id == VehicleID{0x10000 + i});
		CHECK(restored[i].last_x == static_cast<int>(33000 + (i % 32000)));
		CHECK(restored[i].last_x > 32767);
		CHECK(restored[i].last_x < static_cast<int>(Map::SizeX() * 16));
		CHECK(restored[i].last_y == static_cast<int>((i % 60) * 16 + 8));
		CHECK(restored[i].last_tile == TileXY(tile_x, tile_y));
		CHECK(restored[i].last_tile >= TileIndex{65536});
		CHECK(restored[i].last_tile < TileIndex{Map::Size()});
		CHECK(IsValidTile(restored[i].last_tile));
		CHECK(restored[i].side == 1);
		CHECK(restored[i].currently_visible == (i % 2 == 0));
	}
	/* Check boundary at index 65,535 */
	size_t last_idx = NUM_CONTACTS - 1;
	uint last_tile_x = 100 + (static_cast<uint>(last_idx) % 3800);
	uint last_tile_y = 16 + ((static_cast<uint>(last_idx) / 3800) % 46);
	CHECK(restored[last_idx].vehicle_id == VehicleID{0x10000 + static_cast<uint32_t>(last_idx)});
	CHECK(restored[last_idx].last_x == static_cast<int>(33000 + (last_idx % 32000)));
	CHECK(restored[last_idx].last_x > 32767);
	CHECK(restored[last_idx].last_x < static_cast<int>(Map::SizeX() * 16));
	CHECK(restored[last_idx].last_y == static_cast<int>((last_idx % 60) * 16 + 8));
	CHECK(restored[last_idx].last_tile == TileXY(last_tile_x, last_tile_y));
	CHECK(restored[last_idx].last_tile >= TileIndex{65536});
	CHECK(restored[last_idx].last_tile < TileIndex{Map::Size()});
	CHECK(IsValidTile(restored[last_idx].last_tile));
	CHECK(restored[last_idx].side == 1);
	CHECK(restored[last_idx].currently_visible == (last_idx % 2 == 0));

	/* Clean up: restore default 256x256 test environment */
	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Nearest never-scouted selection: distance and deterministic lowest-TileIndex tie breaking independent of heading")
{
	ResetFogTestEnvironment();
	uint8_t side0 = 0;

	/* Aircraft at pixel (1600, 1600) facing Direction::SW (+X heading) */
	DisasterVehicle *aircraft = SpawnMilitaryAircraft(1600, 1600, Direction::SW, side0);
	REQUIRE(aircraft != nullptr);

	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(side0);
	std::fill(intel.is_scouted.begin(), intel.is_scouted.end(), 1);
	intel.never_scouted_count = 0;

	/* Part A: Nearer outside heading cone vs farther inside heading cone */
	TileIndex tile_outside = TileXY(94, 100);  // dx = 1512 - 1600 = -88, dy = 8 -> dist = 7808 (behind aircraft)
	TileIndex tile_inside  = TileXY(120, 100); // dx = 1928 - 1600 = 328, dy = 8 -> dist = 107648 (forward in cone)
	intel.is_scouted[tile_outside.base()] = 0;
	intel.is_scouted[tile_inside.base()] = 0;
	intel.never_scouted_count = 2;

	/* Selection must choose nearest by distance independent of heading */
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_outside);

	/* Heading in other directions must still return the nearest tile */
	aircraft->direction = Direction::NE;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_outside);
	aircraft->direction = Direction::SE;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_outside);
	aircraft->direction = Direction::NW;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_outside);

	/* Part B: Equal-distance tie-breaking by lowest TileIndex */
	aircraft->x_pos = 1608;
	aircraft->y_pos = 1608; // Center of tile (100, 100)

	intel.is_scouted[tile_outside.base()] = 1;
	intel.is_scouted[tile_inside.base()] = 1;
	intel.never_scouted_count = 0;

	TileIndex tile_tie1 = TileXY(100, 95);  // dy = -80, dist = 6400, lower TileIndex
	TileIndex tile_tie2 = TileXY(100, 105); // dy = +80, dist = 6400, higher TileIndex
	REQUIRE(tile_tie1 < tile_tie2);

	intel.is_scouted[tile_tie1.base()] = 0;
	intel.is_scouted[tile_tie2.base()] = 0;
	intel.never_scouted_count = 2;

	/* Facing towards tile_tie2 (+Y) must still break ties deterministically in favor of lower TileIndex */
	aircraft->direction = Direction::SE;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_tie1);

	aircraft->direction = Direction::NW;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_tie1);

	aircraft->direction = Direction::SW;
	CHECK(MilitaryFog::FindNearestNeverScoutedTile(aircraft) == tile_tie1);

	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Visibility loss invalidates drawn vehicle without manual redraw")
{
	ResetFogTestEnvironment();
	REQUIRE(SetupMilitaryScenario());

	DisasterVehicle *marker = nullptr;
	DisasterVehicle *aircraft = nullptr;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_TARGET_MARKER && v->GetMilitarySide() == 1 && TileX(v->tile) == 140) {
			marker = v;
		} else if (v->subtype == ST_MILITARY_AIRCRAFT && v->GetMilitarySide() == 0) {
			aircraft = v;
		}
	}
	REQUIRE(marker != nullptr);
	REQUIRE(aircraft != nullptr);

	/* Position aircraft directly at tile (140, 100) where enemy marker is located */
	aircraft->x_pos = 140 * TILE_SIZE;
	aircraft->y_pos = 100 * TILE_SIZE;

	/* Run normal vehicle tick: aircraft reveals marker and establishes its drawn bounds */
	CallVehicleTicks();
	CHECK(MilitaryFog::IsTileCurrentlyVisible(0, TileXY(140, 100)));
	CHECK_FALSE(MilitaryFog::ShouldHideVehicleForLocal(marker));

	/* Capture drawn vehicle bounds while visible */
	Rect drawn_bounds = marker->coord;
	REQUIRE(drawn_bounds.left != INVALID_COORD);

	/* Relocate aircraft away to (180, 120) and update its viewport so its movement is handled */
	aircraft->UpdatePosition(180 * TILE_SIZE, 120 * TILE_SIZE, GetAircraftFlightLevel(aircraft));
	if (aircraft->Next() != nullptr) {
		aircraft->Next()->UpdatePosition(180 * TILE_SIZE, 120 * TILE_SIZE, 0);
	}
	aircraft->UpdateViewport(true, false);

	/* Install dirty viewport observation hook specifically for the normal tick */
	static std::vector<Rect> invalidated_rects;
	invalidated_rects.clear();
	_viewport_dirty_hook = [](int left, int top, int right, int bottom) {
		invalidated_rects.push_back(Rect{left, top, right, bottom});
	};

	/* Next normal vehicle tick: visibility update automatically invalidates marker and tile */
	CallVehicleTicks();
	_viewport_dirty_hook = nullptr;

	CHECK_FALSE(MilitaryFog::IsTileCurrentlyVisible(0, TileXY(140, 100)));
	CHECK(MilitaryFog::GetTileFogStatus(0, TileXY(140, 100)) == FOG_PREVIOUSLY_SCOUTED);
	CHECK(MilitaryFog::ShouldHideVehicleForLocal(marker));

	/* Assert that previously drawn vehicle bounds were explicitly invalidated during normal tick */
	bool found_vehicle_invalidation = false;
	for (const auto &r : invalidated_rects) {
		if (r.left <= drawn_bounds.left && r.right >= drawn_bounds.right &&
				r.top <= drawn_bounds.top && r.bottom >= drawn_bounds.bottom) {
			found_vehicle_invalidation = true;
			break;
		}
	}
	CHECK(found_vehicle_invalidation);

	ResetMilitaryScenario();
}

TEST_CASE("MilitaryFog - Initial stationary knowledge preservation across UI surfaces")
{
	ResetFogTestEnvironment();

	/* Create starting town and industry */
	TileIndex town_tile = TileXY(50, 50);
	REQUIRE(Town::CanAllocateItem());
	Town *town = Town::Create(town_tile);
	REQUIRE(town != nullptr);
	town->name = "InitialTown";
	town->cache.population = 1200;
	town->cache.num_houses = 35;
	town->larger_town = true;

	TileIndex ind_tile = TileXY(80, 80);
	REQUIRE(Industry::CanAllocateItem());
	Industry *ind = Industry::Create(ind_tile);
	REQUIRE(ind != nullptr);
	ind->town = town;
	ind->type = 1;
	ind->location.tile = ind_tile;
	ind->owner = OWNER_NONE;
	ind->random_colour = Colours::Blue;
	ind->cached_name = "InitialIndustry";

	/* Initialize fog: creates initial knowledge snapshots of starting entities */
	MilitaryFog::Initialize();

	uint8_t side0 = 0;
	/* Verify tiles are not scouted */
	CHECK(MilitaryFog::GetTileFogStatus(side0, town_tile) == FOG_NEVER_SCOUTED);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, town_tile));
	CHECK(MilitaryFog::GetTileFogStatus(side0, ind_tile) == FOG_NEVER_SCOUTED);
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, ind_tile));

	/* Verify snapshots preserve initial knowledge while unscouted */
	const TownSnapshot *ts = MilitaryFog::GetTownSnapshot(side0, town->index);
	REQUIRE(ts != nullptr);
	CHECK(ts->exists);
	CHECK(ts->population == 1200);
	CHECK(ts->num_houses == 35);
	CHECK(ts->name == "InitialTown");
	CHECK(ts->xy == town_tile);
	CHECK(ts->larger_town);

	const IndustrySnapshot *is = MilitaryFog::GetIndustrySnapshot(side0, ind->index);
	REQUIRE(is != nullptr);
	CHECK(is->exists);
	CHECK(is->type == 1);
	CHECK(is->location == ind_tile);
	CHECK(is->colour == Colours::Blue);
	CHECK(is->name == "InitialIndustry");

	/* Clean up */
	delete ind;
	delete town;
	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Decoupled stale stationary snapshots survive live pool deletion and ID reuse")
{
	ResetFogTestEnvironment();

	/* Initialize fog */
	MilitaryFog::Initialize();

	TileIndex ind_tile_A = TileXY(70, 70);
	TileIndex ind_tile_A2 = ind_tile_A + TileDiffXY(1, 0);

	REQUIRE(Industry::CanAllocateItem());
	Industry *ind_A = Industry::Create(ind_tile_A);
	REQUIRE(ind_A != nullptr);
	ind_A->type = 0;
	ind_A->location.tile = ind_tile_A;
	ind_A->owner = OWNER_NONE;
	ind_A->random_colour = Colours::Red;
	ind_A->cached_name = "StaleIndustryA";
	IndustryID saved_id = ind_A->index;
	MakeIndustry(ind_tile_A, saved_id, 0, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_A);
	SetIndustryConstructionStage(ind_tile_A, 3);
	MakeIndustry(ind_tile_A2, saved_id, 1, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_A2);
	SetIndustryConstructionStage(ind_tile_A2, 3);

	/* Scout the tiles of Industry A */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A2);

	const IndustrySnapshot *is_A = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A);
	REQUIRE(is_A != nullptr);
	uint32_t handle_A = is_A->handle;
	CHECK(handle_A != 0);
	CHECK(is_A->exists);
	CHECK(is_A->type == 0);
	CHECK(is_A->colour == Colours::Red);
	CHECK(is_A->location == ind_tile_A);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2) == is_A);

	/* Check rendering consumer for A */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td{};
		GetTileDesc(ind_tile_A, td);
		CHECK(td.str == GetIndustrySpec(0)->name);
		CHECK(td.owner[0] == OWNER_NONE);
	}

	/* Hidden deletion of A from live pool and clear live tiles */
	delete ind_A;
	MakeClear(ind_tile_A, ClearGround::Grass, 0);
	MakeClear(ind_tile_A2, ClearGround::Grass, 0);
	REQUIRE(Industry::GetIfValid(saved_id) == nullptr);

	/* Unseen replacement at B reusing A's old pool ID */
	TileIndex ind_tile_B = TileXY(95, 95);
	Industry *reused_ind = Industry::CreateAtIndex(saved_id, ind_tile_B);
	REQUIRE(reused_ind != nullptr);
	reused_ind->type = 2;
	reused_ind->location.tile = ind_tile_B;
	reused_ind->owner = OWNER_NONE;
	reused_ind->random_colour = Colours::Green;
	reused_ind->cached_name = "ReusedIndustryB";
	MakeIndustry(ind_tile_B, saved_id, 2, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_B);
	SetIndustryConstructionStage(ind_tile_B, 3);

	/* Observation of B while A remains hidden (A is NOT scouted) */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_B);

	/* Verify distinct identities: remembered A and newly observed B */
	const IndustrySnapshot *is_A_stale = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A);
	REQUIRE(is_A_stale != nullptr);
	CHECK(is_A_stale->handle == handle_A);
	CHECK(is_A_stale->type == 0);
	CHECK(is_A_stale->colour == Colours::Red);
	CHECK(is_A_stale->location == ind_tile_A);

	const IndustrySnapshot *is_B_new = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_B);
	REQUIRE(is_B_new != nullptr);
	uint32_t handle_B = is_B_new->handle;
	CHECK(handle_B != 0);
	CHECK(handle_B != handle_A);
	CHECK(is_B_new->type == 2);
	CHECK(is_B_new->colour == Colours::Green);
	CHECK(is_B_new->location == ind_tile_B);

	/* Actual rendering consumers under ActiveSnapshotScope */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td_A{};
		GetTileDesc(ind_tile_A, td_A);
		CHECK(td_A.str == GetIndustrySpec(0)->name);

		TileDesc td_B{};
		GetTileDesc(ind_tile_B, td_B);
		CHECK(td_B.str == GetIndustrySpec(2)->name);
	}

	/* Pre-rescout save/load: prove distinct identities and side isolation survive load */
	SaveLoadResult save_res = SaveOrLoad("test_pre_rescout.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	MilitaryFog::Reset();
	REQUIRE_FALSE(MilitaryFog::IsActive());

	SaveLoadResult load_res = SaveOrLoad("test_pre_rescout.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_pre_rescout.sav");

	REQUIRE(MilitaryFog::IsActive());

	/* Verify side 0 restored properly */
	const IndustrySnapshot *loaded_A = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A);
	REQUIRE(loaded_A != nullptr);
	CHECK(loaded_A->handle == handle_A);
	CHECK(loaded_A->type == 0);
	CHECK(loaded_A->colour == Colours::Red);
	CHECK(loaded_A->location == ind_tile_A);

	const IndustrySnapshot *loaded_B = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_B);
	REQUIRE(loaded_B != nullptr);
	CHECK(loaded_B->handle == handle_B);
	CHECK(loaded_B->type == 2);
	CHECK(loaded_B->colour == Colours::Green);
	CHECK(loaded_B->location == ind_tile_B);

	/* Verify rendering consumers after load before rescout */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td_A{};
		GetTileDesc(ind_tile_A, td_A);
		CHECK(td_A.str == GetIndustrySpec(0)->name);

		TileDesc td_B{};
		GetTileDesc(ind_tile_B, td_B);
		CHECK(td_B.str == GetIndustrySpec(2)->name);
	}

	/* Verify side isolation: side 1 did not scout either and has no snapshots */
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(1, ind_tile_A) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(1, ind_tile_B) == nullptr);

	/* Re-scout A: removal of remembered industry */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A2);

	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotByHandle(0, handle_A) == nullptr);

	/* B remains intact */
	const IndustrySnapshot *final_B = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_B);
	REQUIRE(final_B != nullptr);
	CHECK(final_B->handle == handle_B);
	CHECK(final_B->type == 2);

	Industry *loaded_ind_B = Industry::GetIfValid(saved_id);
	if (loaded_ind_B != nullptr) delete loaded_ind_B;
	MakeClear(ind_tile_B, ClearGround::Grass, 0);
	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Rescouting discovers industry closure and updates stationary snapshot")
{
	ResetFogTestEnvironment();

	TileIndex ind_tile = TileXY(140, 100);
	REQUIRE(Industry::CanAllocateItem());
	Industry *ind = Industry::Create(ind_tile);
	REQUIRE(ind != nullptr);
	ind->type = 0;
	ind->location.tile = ind_tile;
	ind->owner = OWNER_NONE;
	ind->random_colour = Colours::Yellow;
	IndustryID ind_id = ind->index;
	MakeIndustry(ind_tile, ind_id, 0, 0, WaterClass::Invalid);

	MilitaryFog::Initialize();

	/* Initial scout reveals industry */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile);
	const IndustrySnapshot *is = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile);
	REQUIRE(is != nullptr);
	CHECK(is->exists);

	/* Industry closes under fog */
	delete ind;
	MakeClear(ind_tile, ClearGround::Grass, 0);

	/* Stale snapshot still exists prior to rescouting */
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile) != nullptr);

	/* Rescout tile */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile);

	/* Post-rescout: snapshot confirmed absent */
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile) == nullptr);

	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Full stationary snapshot persistence across save/load with side isolation")
{
	ResetFogTestEnvironment();

	uint8_t side0 = 0;
	uint8_t side1 = 1;

	SideIntelligence &intel0 = MilitaryFog::GetSideIntelligence(side0);
	intel0.town_snapshots.resize(4);
	TownSnapshot &ts0 = intel0.town_snapshots[2];
	ts0.exists = true;
	ts0.population = 4500;
	ts0.num_houses = 120;
	ts0.name = "IsolatedTownSide0";
	ts0.xy = TileXY(110, 110);
	ts0.larger_town = true;

	intel0.next_industry_handle = 2;
	intel0.industry_snapshots.resize(4);
	IndustrySnapshot &is0 = intel0.industry_snapshots[1];
	is0.handle = 1;
	is0.exists = true;
	is0.type = 3;
	is0.location = TileXY(115, 115);
	is0.prod_level = 16;
	is0.owner = OWNER_NONE;
	is0.colour = Colours::DarkBlue;
	is0.name = "IsolatedIndustrySide0";
	is0.observed_tiles.push_back(TileXY(115, 115));
	intel0.tile_to_industry_handle[TileXY(115, 115).base()] = 1;

	/* Side 1 has independent snapshot */
	SideIntelligence &intel1 = MilitaryFog::GetSideIntelligence(side1);
	intel1.town_snapshots.resize(4);
	intel1.industry_snapshots.resize(4);

	SaveLoadResult save_res = SaveOrLoad("test_stationary_persistence.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	MilitaryFog::Reset();
	REQUIRE_FALSE(MilitaryFog::IsActive());

	SaveLoadResult load_res = SaveOrLoad("test_stationary_persistence.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_stationary_persistence.sav");

	REQUIRE(MilitaryFog::IsActive());

	/* Verify side 0 restored */
	const TownSnapshot *restored_ts0 = MilitaryFog::GetTownSnapshot(side0, TownID{2});
	REQUIRE(restored_ts0 != nullptr);
	CHECK(restored_ts0->exists);
	CHECK(restored_ts0->population == 4500);
	CHECK(restored_ts0->num_houses == 120);
	CHECK(restored_ts0->name == "IsolatedTownSide0");
	CHECK(restored_ts0->xy == TileXY(110, 110));
	CHECK(restored_ts0->larger_town);

	const IndustrySnapshot *restored_is0 = MilitaryFog::GetIndustrySnapshotAtTile(side0, TileXY(115, 115));
	REQUIRE(restored_is0 != nullptr);
	CHECK(restored_is0->exists);
	CHECK(restored_is0->handle == 1);
	CHECK(restored_is0->type == 3);
	CHECK(restored_is0->location == TileXY(115, 115));
	CHECK(restored_is0->prod_level == 16);
	CHECK(restored_is0->owner == OWNER_NONE);
	CHECK(restored_is0->colour == Colours::DarkBlue);
	CHECK(restored_is0->name == "IsolatedIndustrySide0");

	/* Verify side isolation: side 1 does not see side 0's snapshots */
	CHECK(MilitaryFog::GetTownSnapshot(side1, TownID{2}) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(side1, TileXY(115, 115)) == nullptr);

	ResetFogTestEnvironment();
}
