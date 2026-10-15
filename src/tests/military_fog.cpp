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
#include "../smallmap_gui.h"
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

	TileIndex ind_tile_A = TileXY(70, 70);
	TileIndex ind_tile_A2 = ind_tile_A + TileDiffXY(1, 0);

	/* Set up Industry A BEFORE MilitaryFog::Initialize() to verify initial-known footprint discovery */
	REQUIRE(Industry::CanAllocateItem());
	Industry *ind_A = Industry::Create(ind_tile_A);
	REQUIRE(ind_A != nullptr);
	ind_A->type = 0; // Coal Mine
	ind_A->location = TileArea(ind_tile_A, 2, 1);
	ind_A->owner = OWNER_NONE;
	ind_A->random_colour = Colours::Red;
	ind_A->cached_name = "InitialIndustryA";
	ind_A->prod_level = 100;
	IndustryID saved_id = ind_A->index;
	Industry::industries[ind_A->type].insert(ind_A->index);

	MakeIndustry(ind_tile_A, saved_id, 0, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_A);
	SetIndustryConstructionStage(ind_tile_A, 3);
	MakeIndustry(ind_tile_A2, saved_id, 1, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_A2);
	SetIndustryConstructionStage(ind_tile_A2, 3);

	/* Initialize fog: discovers existing industries and maps multi-tile footprints */
	MilitaryFog::Initialize();

	/* Verify every initial footprint mapping while tiles remain unscouted */
	CHECK(MilitaryFog::GetTileFogStatus(0, ind_tile_A) == FOG_NEVER_SCOUTED);
	CHECK(MilitaryFog::GetTileFogStatus(0, ind_tile_A2) == FOG_NEVER_SCOUTED);

	const IndustrySnapshot *is_A = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A);
	REQUIRE(is_A != nullptr);
	uint32_t handle_A = is_A->handle;
	CHECK(handle_A != 0);
	CHECK(is_A->exists);
	CHECK(is_A->type == 0);
	CHECK(is_A->colour == Colours::Red);
	CHECK(is_A->location == ind_tile_A);
	CHECK(is_A->prod_level == 100);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2) == is_A);

	/* Initial rendering consumers and selection functions while unscouted */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td_A1{};
		GetTileDesc(ind_tile_A, td_A1);
		CHECK(td_A1.str == GetIndustrySpec(0)->name);
		CHECK(td_A1.owner[0] == OWNER_NONE);

		TileDesc td_A2{};
		GetTileDesc(ind_tile_A2, td_A2);
		CHECK(td_A2.str == GetIndustrySpec(0)->name);
		CHECK(td_A2.owner[0] == OWNER_NONE);

		/* Main-map graphics, palette colour and selectors */
		CHECK(IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A) == 0);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_A) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A) == GetColourPalette(Colours::Red));
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A2) == GetColourPalette(Colours::Red));

		/* Minimap type, colour, count, and formatted legend */
		CHECK(GetSmallMapIndustryType(ind_tile_A) == 0);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == 0);
		CHECK(GetSmallMapIndustryPixel(ind_tile_A) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryPixel(ind_tile_A2) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryCount(0) == 1);

		/* Shared production selector */
		CHECK(MilitaryFog::GetIndustryProduction(0, ind_tile_A) == 100);
	}

	/* Hidden deletion of A from live pool and clear live tiles */
	delete ind_A;
	MakeClear(ind_tile_A, ClearGround::Grass, 0);
	MakeClear(ind_tile_A2, ClearGround::Grass, 0);
	REQUIRE(Industry::GetIfValid(saved_id) == nullptr);
	CHECK(Industry::GetIndustryTypeCount(0) == 0); // Live pool count decremented immediately
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1); // Fog count remains stale!

	/* Unseen replacement at B reusing A's old pool ID */
	TileIndex ind_tile_B = TileXY(95, 95);
	Industry *reused_ind = Industry::CreateAtIndex(saved_id, ind_tile_B);
	REQUIRE(reused_ind != nullptr);
	reused_ind->type = 2; // Sawmill
	reused_ind->location = TileArea(ind_tile_B, 1, 1);
	reused_ind->owner = OWNER_NONE;
	reused_ind->random_colour = Colours::Green;
	reused_ind->cached_name = "ReusedIndustryB";
	Industry::industries[reused_ind->type].insert(reused_ind->index);

	MakeIndustry(ind_tile_B, saved_id, 2, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_B);
	SetIndustryConstructionStage(ind_tile_B, 3);
	CHECK(Industry::GetIndustryTypeCount(2) == 1);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 2) == 0); // Fog does not know B yet!

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

	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1); // Stale A still counted
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 2) == 1); // Observed B now counted

	/* Consumers for stale A and newly observed B */
	{
		ActiveSnapshotScope scope(0);
		/* Stale Industry A: remembered graphics/palette and minimap selection unchanged */
		CHECK(IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A) == 0);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_A) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A) == GetColourPalette(Colours::Red));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == 0);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == 0);

		/* Newly observed Industry B */
		CHECK(IsTileType(ind_tile_B, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_B) == 2);
		CHECK(GetIndustryTileColour(ind_tile_B) == Colours::Green);
		CHECK(GetIndustryTilePalette(ind_tile_B) == GetColourPalette(Colours::Green));
		CHECK(GetSmallMapIndustryType(ind_tile_B) == 2);
		CHECK(GetSmallMapIndustryPixel(ind_tile_B) == static_cast<uint32_t>(GetIndustrySpec(2)->map_colour.p * 0x01010101));

		/* Minimap counts and formatted labels */
		CHECK(GetSmallMapIndustryCount(0) == 1); // Stale Coal Mine A still counted
		CHECK(GetSmallMapIndustryCount(2) == 1); // Newly observed Sawmill B counted
	}

	/* Now setup Industry C to test same-root appearance change and partial observation */
	TileIndex ind_tile_C = TileXY(110, 110);
	TileIndex ind_tile_C2 = ind_tile_C + TileDiffXY(1, 0);

	REQUIRE(Industry::CanAllocateItem());
	Industry *ind_C = Industry::Create(ind_tile_C);
	REQUIRE(ind_C != nullptr);
	ind_C->type = 0; // Coal Mine
	ind_C->location = TileArea(ind_tile_C, 2, 1);
	ind_C->owner = OWNER_NONE;
	ind_C->random_colour = Colours::Red;
	ind_C->cached_name = "TownC Industry";
	ind_C->prod_level = 100;
	Industry::industries[ind_C->type].insert(ind_C->index);

	MakeIndustry(ind_tile_C, ind_C->index, 0, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_C);
	SetIndustryConstructionStage(ind_tile_C, 3);
	MakeIndustry(ind_tile_C2, ind_C->index, 1, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_C2);
	SetIndustryConstructionStage(ind_tile_C2, 3);

	/* Scout both tiles of C initially */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_C);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_C2);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2); // Stale A + C

	/* Change C appearance (simulating town rename changing cached_name, plus colour and production) */
	ind_C->cached_name = "RenamedTown Industry";
	ind_C->random_colour = Colours::Blue;
	ind_C->prod_level = 200;
	ind_C->owner = Owner{1};

	/* Partial observation of C: scout C1 (root) only. C2 remains hidden under fog */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_C);

	/* C1 observed with new attributes, while hidden C2 retains old attributes */
	const IndustrySnapshot *snap_C1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C);
	const IndustrySnapshot *snap_C2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C2);
	REQUIRE(snap_C1 != nullptr);
	REQUIRE(snap_C2 != nullptr);
	CHECK(snap_C1->colour == Colours::Blue);
	CHECK(snap_C2->colour == Colours::Red); // Hidden tile retains its last-observed colour!
	CHECK(snap_C1->prod_level == 200);      // Root observation updated shared production level
	CHECK(snap_C2->prod_level == 100);      // Hidden tile's snapshot record was NOT overwritten

	/* Key check: same-root partial observation counts Industry C as ONE industry, not two! */
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2); // 1 for A + 1 for C = 2
	CHECK(MilitaryFog::GetIndustryProduction(0, ind_tile_C) == 200);

	/* Rendering consumers during mixed old/new footprint state */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td_C1{};
		GetTileDesc(ind_tile_C, td_C1);
		CHECK(td_C1.str == GetIndustrySpec(0)->name);
		CHECK(td_C1.owner[0] == Owner{1});

		TileDesc td_C2{};
		GetTileDesc(ind_tile_C2, td_C2);
		CHECK(td_C2.str == GetIndustrySpec(0)->name);
		CHECK(td_C2.owner[0] == OWNER_NONE); // Hidden tile keeps old owner in tile desc!

		/* Main-map graphics, colours and palettes */
		CHECK(IsTileType(ind_tile_C, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C) == 0);
		CHECK(IsTileType(ind_tile_C2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_C) == Colours::Blue);
		CHECK(GetIndustryTilePalette(ind_tile_C) == GetColourPalette(Colours::Blue));
		CHECK(GetIndustryTileColour(ind_tile_C2) == Colours::Red); // Hidden tile retains last observed colour/palette!
		CHECK(GetIndustryTilePalette(ind_tile_C2) == GetColourPalette(Colours::Red));

		/* Minimap type, pixel colour, counts and formatted legend entries */
		CHECK(GetSmallMapIndustryType(ind_tile_C) == 0);
		CHECK(GetSmallMapIndustryType(ind_tile_C2) == 0);
		CHECK(GetSmallMapIndustryPixel(ind_tile_C) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryPixel(ind_tile_C2) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryCount(0) == 2); // 1 for A + 1 for C = 2
		CHECK(GetSmallMapIndustryCount(2) == 1); // 1 for B
	}

	/* Save game state for uninterrupted vs loaded comparison with mixed old/new footprint state */
	SaveLoadResult save_res = SaveOrLoad("test_continuation.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	/* --- PATH 1: Uninterrupted continuation --- */
	/* 1. Partial observation of A: scout tile A1 only. A1 is cleared; tile A2 remains hidden */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A);

	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	const IndustrySnapshot *unint_A2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2);
	REQUIRE(unint_A2 != nullptr);
	CHECK(unint_A2->handle == handle_A);
	CHECK(unint_A2->type == 0);
	CHECK(unint_A2->colour == Colours::Red);
	CHECK(unint_A2->exists);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2); // A still has an unobserved tile, so count remains 2 (A + C)

	/* Partial observation consumers: A1 cleared, A2 hidden retains remembered state */
	{
		ActiveSnapshotScope scope(0);
		CHECK(!IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == IT_INVALID);
		CHECK(GetIndustryTileColour(ind_tile_A) == Colours::Grey);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == 0);
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A2) == GetColourPalette(Colours::Red));
		TileDesc td_A2{};
		GetTileDesc(ind_tile_A2, td_A2);
		CHECK(td_A2.str == GetIndustrySpec(0)->name);
		CHECK(GetSmallMapIndustryCount(0) == 2);
	}

	/* 2. Full observation of A: scout tile A2. Now all footprint tiles of A are clear */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A2);

	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotByHandle(0, handle_A) == nullptr);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1); // A removed, only C remains!

	/* Full observation consumers: all tiles of A cleared */
	{
		ActiveSnapshotScope scope(0);
		CHECK(!IsTileType(ind_tile_A, TileType::Industry));
		CHECK(!IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == IT_INVALID);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == IT_INVALID);
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Grey);
		CHECK(GetSmallMapIndustryCount(0) == 1);
	}

	/* 3. Scout C2: now C2 is also observed with Blue */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_C2);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C2)->colour == Colours::Blue);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 2) == 1); // B still intact

	/* Observation of C2 consumers */
	{
		ActiveSnapshotScope scope(0);
		CHECK(IsTileType(ind_tile_C, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C) == 0);
		CHECK(IsTileType(ind_tile_C2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_C2) == Colours::Blue);
		CHECK(GetIndustryTilePalette(ind_tile_C2) == GetColourPalette(Colours::Blue));
		CHECK(GetSmallMapIndustryType(ind_tile_C2) == 0);
		CHECK(GetSmallMapIndustryCount(0) == 1);
		CHECK(GetSmallMapIndustryCount(2) == 1);
	}

	/* --- PATH 2: Loaded continuation --- */
	MilitaryFog::Reset();
	REQUIRE_FALSE(MilitaryFog::IsActive());

	SaveLoadResult load_res = SaveOrLoad("test_continuation.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_continuation.sav");

	REQUIRE(MilitaryFog::IsActive());

	/* Verify side 0 loaded state matches uninterrupted pre-rescout state */
	const IndustrySnapshot *loaded_A1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A);
	REQUIRE(loaded_A1 != nullptr);
	CHECK(loaded_A1->handle == handle_A);
	CHECK(loaded_A1->type == 0);
	CHECK(loaded_A1->colour == Colours::Red);

	const IndustrySnapshot *loaded_A2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2);
	REQUIRE(loaded_A2 != nullptr);
	CHECK(loaded_A2->handle == handle_A);
	CHECK(loaded_A2->type == 0);
	CHECK(loaded_A2->colour == Colours::Red);

	const IndustrySnapshot *loaded_B = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_B);
	REQUIRE(loaded_B != nullptr);
	CHECK(loaded_B->handle == handle_B);
	CHECK(loaded_B->type == 2);
	CHECK(loaded_B->colour == Colours::Green);

	/* Verify mixed old/new footprint of C survived load intact */
	const IndustrySnapshot *loaded_C1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C);
	const IndustrySnapshot *loaded_C2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C2);
	REQUIRE(loaded_C1 != nullptr);
	REQUIRE(loaded_C2 != nullptr);
	CHECK(loaded_C1->colour == Colours::Blue);
	CHECK(loaded_C2->colour == Colours::Red);
	CHECK(loaded_C1->prod_level == 200);
	CHECK(loaded_C2->prod_level == 100);

	/* Consumers after load before re-scout */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td_C1{};
		GetTileDesc(ind_tile_C, td_C1);
		CHECK(td_C1.str == GetIndustrySpec(0)->name);
		CHECK(td_C1.owner[0] == Owner{1});

		TileDesc td_C2{};
		GetTileDesc(ind_tile_C2, td_C2);
		CHECK(td_C2.str == GetIndustrySpec(0)->name);
		CHECK(td_C2.owner[0] == OWNER_NONE);

		/* Main-map graphics, colours and palettes */
		CHECK(IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A) == 0);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(IsTileType(ind_tile_B, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_B) == 2);
		CHECK(IsTileType(ind_tile_C, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C) == 0);
		CHECK(IsTileType(ind_tile_C2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_C) == Colours::Blue);
		CHECK(GetIndustryTilePalette(ind_tile_C) == GetColourPalette(Colours::Blue));
		CHECK(GetIndustryTileColour(ind_tile_C2) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_C2) == GetColourPalette(Colours::Red));

		/* Minimap type, pixel colour, counts and formatted legend entries */
		CHECK(GetSmallMapIndustryType(ind_tile_C) == 0);
		CHECK(GetSmallMapIndustryType(ind_tile_C2) == 0);
		CHECK(GetSmallMapIndustryPixel(ind_tile_C) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryPixel(ind_tile_C2) == static_cast<uint32_t>(GetIndustrySpec(0)->map_colour.p * 0x01010101));
		CHECK(GetSmallMapIndustryCount(0) == 2);
		CHECK(GetSmallMapIndustryCount(2) == 1);

		CHECK(MilitaryFog::GetIndustryProduction(0, ind_tile_C) == 200);
	}

	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 2) == 1);

	/* Verify side isolation: side 1 did not scout B or C and has no snapshots for them */
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(1, ind_tile_B) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(1, ind_tile_C) == nullptr);
	CHECK(MilitaryFog::GetIndustryTypeCount(1, 2) == 0);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(1, ind_tile_A) != nullptr);
	CHECK(MilitaryFog::GetIndustryTypeCount(1, 0) == 1);

	{
		ActiveSnapshotScope scope_side1(1);
		CHECK(!IsTileType(ind_tile_B, TileType::Industry));
		CHECK(!IsTileType(ind_tile_C, TileType::Industry));
		CHECK(IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A) == 0);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetSmallMapIndustryType(ind_tile_B) == IT_INVALID);
		CHECK(GetSmallMapIndustryType(ind_tile_C) == IT_INVALID);
		CHECK(GetSmallMapIndustryCount(0) == 1);
		CHECK(GetSmallMapIndustryCount(2) == 0);
	}

	/* Perform identical partial observation of A on loaded continuation */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2)->colour == Colours::Red);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2);

	{
		ActiveSnapshotScope scope(0);
		CHECK(!IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == IT_INVALID);
		CHECK(GetIndustryTileColour(ind_tile_A) == Colours::Grey);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == 0);
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A2) == GetColourPalette(Colours::Red));
		TileDesc td_A2{};
		GetTileDesc(ind_tile_A2, td_A2);
		CHECK(td_A2.str == GetIndustrySpec(0)->name);
		CHECK(GetSmallMapIndustryCount(0) == 2);
	}

	/* Save and reload during partial footprint observation to prove persistence */
	SaveLoadResult part_save_res = SaveOrLoad("test_partial.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(part_save_res == SaveLoadResult::Ok);

	MilitaryFog::Reset();
	SaveLoadResult part_load_res = SaveOrLoad("test_partial.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(part_load_res == SaveLoadResult::Ok);
	std::remove("test_partial.sav");

	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	const IndustrySnapshot *reloaded_A2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2);
	REQUIRE(reloaded_A2 != nullptr);
	CHECK(reloaded_A2->handle == handle_A);
	CHECK(reloaded_A2->type == 0);
	CHECK(reloaded_A2->colour == Colours::Red);
	CHECK(reloaded_A2->exists);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 2);

	{
		ActiveSnapshotScope scope(0);
		CHECK(!IsTileType(ind_tile_A, TileType::Industry));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == IT_INVALID);
		CHECK(GetIndustryTileColour(ind_tile_A) == Colours::Grey);
		CHECK(IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_A2) == 1);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == 0);
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Red);
		CHECK(GetIndustryTilePalette(ind_tile_A2) == GetColourPalette(Colours::Red));
		TileDesc td_A2{};
		GetTileDesc(ind_tile_A2, td_A2);
		CHECK(td_A2.str == GetIndustrySpec(0)->name);
		CHECK(GetSmallMapIndustryCount(0) == 2);
	}

	/* Perform identical full observation of A */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_A2);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_A2) == nullptr);
	CHECK(MilitaryFog::GetIndustrySnapshotByHandle(0, handle_A) == nullptr);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1);

	{
		ActiveSnapshotScope scope(0);
		CHECK(!IsTileType(ind_tile_A, TileType::Industry));
		CHECK(!IsTileType(ind_tile_A2, TileType::Industry));
		CHECK(GetSmallMapIndustryType(ind_tile_A) == IT_INVALID);
		CHECK(GetSmallMapIndustryType(ind_tile_A2) == IT_INVALID);
		CHECK(GetIndustryTileColour(ind_tile_A2) == Colours::Grey);
		CHECK(GetSmallMapIndustryCount(0) == 1);
	}

	/* Perform identical observation of C2 */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_C2);
	CHECK(MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_C2)->colour == Colours::Blue);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 0) == 1);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 2) == 1);

	{
		ActiveSnapshotScope scope(0);
		CHECK(IsTileType(ind_tile_C, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C) == 0);
		CHECK(IsTileType(ind_tile_C2, TileType::Industry));
		CHECK(GetIndustryGfx(ind_tile_C2) == 1);
		CHECK(GetIndustryTileColour(ind_tile_C2) == Colours::Blue);
		CHECK(GetIndustryTilePalette(ind_tile_C2) == GetColourPalette(Colours::Blue));
		CHECK(GetSmallMapIndustryType(ind_tile_C2) == 0);
		CHECK(GetSmallMapIndustryCount(0) == 1);
		CHECK(GetSmallMapIndustryCount(2) == 1);
	}

	/* Clean up */
	Industry *loaded_ind_B = Industry::GetIfValid(saved_id);
	if (loaded_ind_B != nullptr) delete loaded_ind_B;
	MakeClear(ind_tile_B, ClearGround::Grass, 0);

	Industry *loaded_ind_C = Industry::GetByTileIfValid(ind_tile_C);
	if (loaded_ind_C != nullptr) delete loaded_ind_C;
	MakeClear(ind_tile_C, ClearGround::Grass, 0);
	MakeClear(ind_tile_C2, ClearGround::Grass, 0);

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

TEST_CASE("MilitaryFog - Owner-only per-tile version isolation, vector growth, and save/load continuation")
{
	ResetFogTestEnvironment();

	TileIndex ind_tile_D = TileXY(120, 120);
	TileIndex ind_tile_D2 = ind_tile_D + TileDiffXY(1, 0);

	REQUIRE(Industry::CanAllocateItem());
	Industry *ind_D = Industry::Create(ind_tile_D);
	REQUIRE(ind_D != nullptr);
	ind_D->type = 1; // Power Station
	ind_D->location = TileArea(ind_tile_D, 2, 1);
	ind_D->owner = OWNER_NONE;
	ind_D->random_colour = Colours::Yellow;
	ind_D->prod_level = 100;
	Industry::industries[ind_D->type].insert(ind_D->index);

	MakeIndustry(ind_tile_D, ind_D->index, 0, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_D);
	SetIndustryConstructionStage(ind_tile_D, 3);
	MakeIndustry(ind_tile_D2, ind_D->index, 1, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_D2);
	SetIndustryConstructionStage(ind_tile_D2, 3);

	MilitaryFog::Initialize();

	/* Initial observation of both tiles under fog */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D2);

	const IndustrySnapshot *snap_D_init1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D);
	const IndustrySnapshot *snap_D_init2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D2);
	REQUIRE(snap_D_init1 != nullptr);
	REQUIRE(snap_D_init2 != nullptr);
	CHECK(snap_D_init1 == snap_D_init2);
	CHECK(snap_D_init1->owner == OWNER_NONE);
	CHECK(snap_D_init1->colour == Colours::Yellow);
	CHECK(snap_D_init1->type == 1);
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 1) == 1);

	{
		ActiveSnapshotScope scope(0);
		TileDesc td1{};
		GetTileDesc(ind_tile_D, td1);
		CHECK(td1.owner[0] == OWNER_NONE);
		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == OWNER_NONE);
	}

	/* 1. Pure owner change: name, colour, and type stay identical */
	ind_D->owner = Owner{2};
	ind_D->prod_level = 150; // Root observation updates shared production level

	/* Root-only observation: scout D (root) while D2 remains hidden */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D);

	/* Root has new owner; hidden tile retains old owner */
	const IndustrySnapshot *snap_D1_new = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D);
	const IndustrySnapshot *snap_D2_old = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D2);
	REQUIRE(snap_D1_new != nullptr);
	REQUIRE(snap_D2_old != nullptr);
	CHECK(snap_D1_new != snap_D2_old);
	CHECK(snap_D1_new->owner == Owner{2});
	CHECK(snap_D2_old->owner == OWNER_NONE);
	CHECK(snap_D1_new->colour == Colours::Yellow);
	CHECK(snap_D2_old->colour == Colours::Yellow);
	CHECK(snap_D1_new->prod_level == 150);
	CHECK(snap_D2_old->prod_level == 100);

	/* GetTileDesc reflects per-tile version isolation: D1 new owner, D2 old owner */
	{
		ActiveSnapshotScope scope(0);
		TileDesc td1{};
		GetTileDesc(ind_tile_D, td1);
		CHECK(td1.owner[0] == Owner{2});

		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == OWNER_NONE);
	}

	/* Retain single logical industry count for Industry D */
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 1) == 1);

	/* 2. Repeated root observation: must not rejoin or mutate old shared record */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D);
	{
		ActiveSnapshotScope scope(0);
		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == OWNER_NONE); // Hidden tile still retains old owner
	}
	CHECK(MilitaryFog::GetIndustryTypeCount(0, 1) == 1);

	/* 3. Save and loaded-vs-uninterrupted continuation of mixed owner state */
	SaveLoadResult save_res = SaveOrLoad("test_owner_mixed.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	/* Path 1: Uninterrupted continuation - observe second tile D2 */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D2);
	const IndustrySnapshot *unint_D2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D2);
	REQUIRE(unint_D2 != nullptr);
	CHECK(unint_D2->owner == Owner{2});
	CHECK(unint_D2->handle == snap_D1_new->handle);
	{
		ActiveSnapshotScope scope(0);
		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == Owner{2}); // D2 refreshed to new owner
	}
	CHECK(snap_D2_old->exists == false); // Old snapshot retired

	/* Path 2: Loaded continuation */
	MilitaryFog::Reset();
	REQUIRE_FALSE(MilitaryFog::IsActive());

	SaveLoadResult load_res = SaveOrLoad("test_owner_mixed.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_owner_mixed.sav");

	REQUIRE(MilitaryFog::IsActive());

	/* Immediately after load: mixed owner state is preserved */
	const IndustrySnapshot *loaded_D1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D);
	const IndustrySnapshot *loaded_D2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D2);
	REQUIRE(loaded_D1 != nullptr);
	REQUIRE(loaded_D2 != nullptr);
	CHECK(loaded_D1->owner == Owner{2});
	CHECK(loaded_D2->owner == OWNER_NONE);
	{
		ActiveSnapshotScope scope(0);
		TileDesc td1{};
		GetTileDesc(ind_tile_D, td1);
		CHECK(td1.owner[0] == Owner{2});
		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == OWNER_NONE);
	}

	/* Observe second tile D2 on loaded continuation */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_D2);
	const IndustrySnapshot *loaded_rescout_D2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_D2);
	REQUIRE(loaded_rescout_D2 != nullptr);
	CHECK(loaded_rescout_D2->owner == Owner{2});
	CHECK(loaded_rescout_D2->handle == loaded_D1->handle);
	{
		ActiveSnapshotScope scope(0);
		TileDesc td2{};
		GetTileDesc(ind_tile_D2, td2);
		CHECK(td2.owner[0] == Owner{2});
	}
	CHECK(loaded_D2->exists == false); // Old snapshot retired

	/* Clean up D */
	Industry *loaded_ind_D = Industry::GetByTileIfValid(ind_tile_D);
	if (loaded_ind_D != nullptr) delete loaded_ind_D;
	MakeClear(ind_tile_D, ClearGround::Grass, 0);
	MakeClear(ind_tile_D2, ClearGround::Grass, 0);

	/* 4. Vector growth regression inside production RefreshStationarySnapshot path */
	TileIndex ind_tile_E = TileXY(130, 130);
	TileIndex ind_tile_E2 = ind_tile_E + TileDiffXY(1, 0);
	TileIndex ind_tile_E3 = ind_tile_E + TileDiffXY(2, 0);

	REQUIRE(Industry::CanAllocateItem());
	Industry *ind_E = Industry::Create(ind_tile_E);
	REQUIRE(ind_E != nullptr);
	ind_E->type = 2; // Sawmill
	ind_E->location = TileArea(ind_tile_E, 3, 1);
	ind_E->owner = OWNER_NONE;
	ind_E->random_colour = Colours::Green;
	ind_E->prod_level = 100;
	Industry::industries[ind_E->type].insert(ind_E->index);

	MakeIndustry(ind_tile_E, ind_E->index, 0, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_E);
	SetIndustryConstructionStage(ind_tile_E, 3);
	MakeIndustry(ind_tile_E2, ind_E->index, 1, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_E2);
	SetIndustryConstructionStage(ind_tile_E2, 3);
	MakeIndustry(ind_tile_E3, ind_E->index, 2, 0, WaterClass::Invalid);
	SetIndustryCompleted(ind_tile_E3);
	SetIndustryConstructionStage(ind_tile_E3, 3);

	/* Scout all three tiles initially into a shared version */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_E);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_E2);
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_E3);

	const IndustrySnapshot *snap_E_init = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_E);
	REQUIRE(snap_E_init != nullptr);
	CHECK(snap_E_init->prod_level == 100);
	CHECK(snap_E_init->owner == OWNER_NONE);

	/* Control capacity using existing access: pad inactive entries up to capacity so the next snapshot allocation MUST reallocate */
	SideIntelligence &intel = MilitaryFog::GetSideIntelligence(0);
	while (intel.industry_snapshots.size() < intel.industry_snapshots.capacity()) {
		intel.industry_snapshots.emplace_back();
	}
	size_t cap_before = intel.industry_snapshots.capacity();

	/* Live industry undergoes owner and production change */
	ind_E->owner = Owner{3};
	ind_E->prod_level = 250;

	/* Observe non-root tile E2 while root E and sibling E3 remain hidden under fog.
	 * This forces a non-root appearance allocation inside RefreshStationarySnapshot,
	 * reallocating industry_snapshots vector while prior snapshot is in use and retaining prev_prod. */
	MilitaryFog::RefreshStationarySnapshot(0, ind_tile_E2);

	/* Verify vector capacity demonstrably grew inside RefreshStationarySnapshot */
	CHECK(intel.industry_snapshots.capacity() > cap_before);

	/* Reacquire test pointers safely after reallocation */
	const IndustrySnapshot *snap_E2 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_E2);
	REQUIRE(snap_E2 != nullptr);
	CHECK(snap_E2->owner == Owner{3});
	CHECK(snap_E2->prod_level == 100); // Retained prior production, NOT live root production (250)

	/* Verify still-hidden siblings retain prior description and state */
	const IndustrySnapshot *snap_E1 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_E);
	const IndustrySnapshot *snap_E3 = MilitaryFog::GetIndustrySnapshotAtTile(0, ind_tile_E3);
	REQUIRE(snap_E1 != nullptr);
	REQUIRE(snap_E3 != nullptr);
	CHECK(snap_E1->owner == OWNER_NONE);
	CHECK(snap_E3->owner == OWNER_NONE);
	CHECK(snap_E1->prod_level == 100);
	CHECK(snap_E3->prod_level == 100);

	{
		ActiveSnapshotScope scope(0);
		TileDesc td_E2{};
		GetTileDesc(ind_tile_E2, td_E2);
		CHECK(td_E2.owner[0] == Owner{3});

		TileDesc td_E1{};
		GetTileDesc(ind_tile_E, td_E1);
		CHECK(td_E1.owner[0] == OWNER_NONE);

		TileDesc td_E3{};
		GetTileDesc(ind_tile_E3, td_E3);
		CHECK(td_E3.owner[0] == OWNER_NONE);
	}

	/* Clean up E */
	delete ind_E;
	MakeClear(ind_tile_E, ClearGround::Grass, 0);
	MakeClear(ind_tile_E2, ClearGround::Grass, 0);
	MakeClear(ind_tile_E3, ClearGround::Grass, 0);

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
	ts0.handle = 1;
	ts0.original_id = TownID{2};
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

TEST_CASE("MilitaryFog - Town main-map and minimap labels from side intelligence with hidden changes and rescout")
{
	ResetFogTestEnvironment();
	_settings_client.gui.population_in_label = true;

	uint8_t side0 = 0;
	uint8_t side1 = 1;

	/* Create starting town */
	TileIndex town_tile = TileXY(70, 70);
	REQUIRE(Town::CanAllocateItem());
	Town *town = Town::Create(town_tile);
	REQUIRE(town != nullptr);
	town->name = "Verdun";
	town->townnametype = SPECSTR_TOWNNAME_START;
	town->cache.population = 2500;
	town->cache.num_houses = 70;
	town->larger_town = false;

	/* Initialize fog: creates initial knowledge snapshots of starting towns */
	MilitaryFog::Initialize();

	/* 1. Initial snapshot availability without marking tiles explored */
	CHECK_FALSE(MilitaryFog::IsTileScouted(side0, town_tile));
	CHECK_FALSE(MilitaryFog::IsTileScouted(side1, town_tile));

	const TownSnapshot *ts0_init = MilitaryFog::GetTownSnapshotAtTile(side0, town_tile);
	REQUIRE(ts0_init != nullptr);
	CHECK(ts0_init->exists);
	CHECK(ts0_init->name == "Verdun");
	CHECK(ts0_init->population == 2500);
	CHECK(ts0_init->num_houses == 70);
	CHECK_FALSE(ts0_init->larger_town);
	CHECK(ts0_init->sign.width_normal > 0);
	CHECK(ts0_init->sign.width_small > 0);

	/* Check viewport and smallmap string retrieval */
	CHECK(ts0_init->GetViewportString(true) == "Verdun");
	CHECK(ts0_init->GetViewportString(false).find("Verdun") != std::string::npos);
	CHECK((ts0_init->GetViewportString(false).find("2,500") != std::string::npos || ts0_init->GetViewportString(false).find("2500") != std::string::npos));

	/* Under ActiveSnapshotScope, side 0 and side 1 see initial snapshot data */
	{
		ActiveSnapshotScope scope(side0);
		const TownSnapshot *ts_scope = MilitaryFog::GetTownSnapshotAtTile(side0, town_tile);
		REQUIRE(ts_scope != nullptr);
		CHECK(ts_scope->name == "Verdun");
		CHECK(ts_scope->population == 2500);
	}

	/* 2. Hidden town changes under fog (population growth, city status, rename) */
	town->name = "Verdun Fortress";
	town->cache.population = 8900;
	town->cache.num_houses = 210;
	town->larger_town = true;

	/* Unscouted side 0 retains stale initial snapshot */
	const TownSnapshot *ts0_stale = MilitaryFog::GetTownSnapshotAtTile(side0, town_tile);
	REQUIRE(ts0_stale != nullptr);
	CHECK(ts0_stale->name == "Verdun");
	CHECK(ts0_stale->population == 2500);
	CHECK(ts0_stale->num_houses == 70);
	CHECK_FALSE(ts0_stale->larger_town);
	CHECK(ts0_stale->GetViewportString(true) == "Verdun");
	CHECK((ts0_stale->GetViewportString(false).find("2,500") != std::string::npos || ts0_stale->GetViewportString(false).find("2500") != std::string::npos));
	CHECK(ts0_stale->GetViewportString(false).find("8900") == std::string::npos);
	CHECK(ts0_stale->GetViewportString(false).find("8,900") == std::string::npos);

	/* Spectator / non-fog observer sees live town data */
	{
		ActiveSnapshotScope spectator_scope(MAX_MILITARY_SIDES);
		CHECK(town->name == "Verdun Fortress");
		CHECK(town->cache.population == 8900);
		CHECK(town->larger_town);
	}

	/* 3. Side isolation: side 1 scouts the town while side 0 remains hidden */
	MilitaryFog::RefreshStationarySnapshot(side1, town_tile);

	const TownSnapshot *ts1_scouted = MilitaryFog::GetTownSnapshotAtTile(side1, town_tile);
	REQUIRE(ts1_scouted != nullptr);
	CHECK(ts1_scouted->name == "Verdun Fortress");
	CHECK(ts1_scouted->population == 8900);
	CHECK(ts1_scouted->num_houses == 210);
	CHECK(ts1_scouted->larger_town);
	CHECK(ts1_scouted->GetViewportString(true) == "Verdun Fortress");
	CHECK((ts1_scouted->GetViewportString(false).find("8,900") != std::string::npos || ts1_scouted->GetViewportString(false).find("8900") != std::string::npos));

	/* Side 0 snapshot remains strictly isolated and unchanged */
	const TownSnapshot *ts0_still_stale = MilitaryFog::GetTownSnapshotAtTile(side0, town_tile);
	REQUIRE(ts0_still_stale != nullptr);
	CHECK(ts0_still_stale->name == "Verdun");
	CHECK(ts0_still_stale->population == 2500);
	CHECK_FALSE(ts0_still_stale->larger_town);

	/* 4. Side 0 rescouts the town via aircraft */
	DisasterVehicle *scout0 = SpawnMilitaryAircraft(70 * TILE_SIZE, 70 * TILE_SIZE, Direction::E, side0);
	REQUIRE(scout0 != nullptr);
	scout0->Tick();

	const TownSnapshot *ts0_rescouted = MilitaryFog::GetTownSnapshotAtTile(side0, town_tile);
	REQUIRE(ts0_rescouted != nullptr);
	CHECK(ts0_rescouted->name == "Verdun Fortress");
	CHECK(ts0_rescouted->population == 8900);
	CHECK(ts0_rescouted->num_houses == 210);
	CHECK(ts0_rescouted->larger_town);
	CHECK(ts0_rescouted->GetViewportString(true) == "Verdun Fortress");
	CHECK((ts0_rescouted->GetViewportString(false).find("8,900") != std::string::npos || ts0_rescouted->GetViewportString(false).find("8900") != std::string::npos));

	delete town;
	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Decoupled town identity across hidden deletion and TownID pool reuse")
{
	ResetFogTestEnvironment();

	uint8_t side0 = 0;

	/* Create initial town A */
	TileIndex tile_A = TileXY(40, 40);
	REQUIRE(Town::CanAllocateItem());
	Town *town_A = Town::Create(tile_A);
	REQUIRE(town_A != nullptr);
	town_A->name = "Metz";
	town_A->cache.population = 3000;
	TownID id_A = town_A->index;

	MilitaryFog::Initialize();

	const TownSnapshot *ts_A_init = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	REQUIRE(ts_A_init != nullptr);
	CHECK(ts_A_init->name == "Metz");
	uint32_t handle_A = ts_A_init->handle;

	/* Delete Town A under fog (e.g. wiped out or cleared) */
	delete town_A;

	/* Allocate Town B. With Town A deleted, pool reuses TownID id_A */
	TileIndex tile_B = TileXY(90, 90);
	REQUIRE(Town::CanAllocateItem());
	Town *town_B = Town::Create(tile_B);
	REQUIRE(town_B != nullptr);
	CHECK(town_B->index == id_A); // Pool ID reused!
	town_B->name = "Sedan";
	town_B->cache.population = 4200;

	/* Side 0 has not scouted tile A or tile B.
	 * Town A's snapshot at tile A must remain intact and must NOT be redirected to tile B */
	const TownSnapshot *ts_A_hidden = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	REQUIRE(ts_A_hidden != nullptr);
	CHECK(ts_A_hidden->exists);
	CHECK(ts_A_hidden->handle == handle_A);
	CHECK(ts_A_hidden->xy == tile_A);
	CHECK(ts_A_hidden->name == "Metz");
	CHECK(ts_A_hidden->population == 3000);

	/* Tile B has no snapshot for side 0 yet */
	CHECK(MilitaryFog::GetTownSnapshotAtTile(side0, tile_B) == nullptr);

	/* Side 0 scouts tile A: discovers Town A has been removed */
	MilitaryFog::RefreshStationarySnapshot(side0, tile_A);
	const TownSnapshot *ts_A_discovered = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	CHECK(ts_A_discovered == nullptr); // Exists flag is now false

	/* Side 0 scouts tile B: discovers Town B */
	MilitaryFog::RefreshStationarySnapshot(side0, tile_B);
	const TownSnapshot *ts_B_scouted = MilitaryFog::GetTownSnapshotAtTile(side0, tile_B);
	REQUIRE(ts_B_scouted != nullptr);
	CHECK(ts_B_scouted->exists);
	CHECK(ts_B_scouted->xy == tile_B);
	CHECK(ts_B_scouted->name == "Sedan");
	CHECK(ts_B_scouted->population == 4200);
	CHECK(ts_B_scouted->handle != handle_A); // New handle assigned

	delete town_B;
	ResetFogTestEnvironment();
}

TEST_CASE("MilitaryFog - Town snapshot save/load persistence and continuation equivalence")
{
	ResetFogTestEnvironment();
	extern void ResetHouses();
	ResetHouses();
	extern void InitializeLanguagePacks();
	InitializeLanguagePacks();
	_settings_client.gui.population_in_label = true;

	uint8_t side0 = 0;
	uint8_t side1 = 1;
	TileIndex tile_A = TileXY(55, 55);
	TileIndex tile_B = TileXY(85, 85);
	TileIndex tile_C = TileXY(105, 105);

	/* Initialize scenario with Town A and Town B */
	REQUIRE(Town::CanAllocateItem());
	Town *town_A = Town::Create(tile_A);
	REQUIRE(town_A != nullptr);
	town_A->name = "Verdun";
	town_A->townnametype = SPECSTR_TOWNNAME_START;
	town_A->cache.population = 1200;
	town_A->larger_town = false;

	REQUIRE(Town::CanAllocateItem());
	Town *town_B = Town::Create(tile_B);
	REQUIRE(town_B != nullptr);
	town_B->name = "Metz";
	town_B->townnametype = SPECSTR_TOWNNAME_START;
	town_B->cache.population = 800;
	town_B->larger_town = false;

	TownID id_B = town_B->index;

	MilitaryFog::Initialize();

	/* Verify both sides start with initial snapshot of A and B */
	const TownSnapshot *snap_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	const TownSnapshot *snap_B0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_B);
	const TownSnapshot *snap_A1 = MilitaryFog::GetTownSnapshotAtTile(side1, tile_A);
	REQUIRE(snap_A0 != nullptr);
	REQUIRE(snap_B0 != nullptr);
	REQUIRE(snap_A1 != nullptr);
	uint32_t handle_A0 = snap_A0->handle;
	uint32_t handle_B0 = snap_B0->handle;

	/* Create side differentiation under fog:
	 * Live Town A is renamed to "Verdun Fortress", pop 3500, city flag true.
	 * Side 0 has NOT scouted Town A -> Side 0 retains stale "Verdun", pop 1200.
	 * Side 1 scouts Town A -> Side 1 refreshed to "Verdun Fortress", pop 3500, city true.
	 */
	town_A->name = "Verdun Fortress";
	town_A->cache.population = 3500;
	town_A->larger_town = true;
	town_A->UpdateVirtCoord();

	MilitaryFog::RefreshStationarySnapshot(side1, tile_A);

	/* Create identity case under fog:
	 * Live Town B is deleted under fog. Neither side has scouted Tile B yet.
	 * Both sides retain stale Town B snapshot at Tile B.
	 */
	delete town_B;

	/* Verify side differentiation and stale identity before saving */
	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side0, tile_A)->name == "Verdun");
	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side0, tile_A)->population == 1200);
	REQUIRE_FALSE(MilitaryFog::GetTownSnapshotAtTile(side0, tile_A)->larger_town);

	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side1, tile_A)->name == "Verdun Fortress");
	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side1, tile_A)->population == 3500);
	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side1, tile_A)->larger_town);

	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side0, tile_B)->exists);
	REQUIRE(MilitaryFog::GetTownSnapshotAtTile(side0, tile_B)->name == "Metz");

	/* Save state */
	SaveLoadResult save_res = SaveOrLoad("test_town_labels_continuation.sav", SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(save_res == SaveLoadResult::Ok);

	/* --- PATH 1: Uninterrupted continuation --- */
	/* Consumer values before continuation on uninterrupted branch */
	std::string unint_pre_main_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A)->GetViewportString(false);
	std::string unint_pre_mini_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A)->GetViewportString(true);
	std::string unint_pre_main_A1 = MilitaryFog::GetTownSnapshotAtTile(side1, tile_A)->GetViewportString(false);
	std::string unint_pre_mini_A1 = MilitaryFog::GetTownSnapshotAtTile(side1, tile_A)->GetViewportString(true);
	std::string unint_pre_main_B0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_B)->GetViewportString(false);
	std::string unint_pre_mini_B0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_B)->GetViewportString(true);

	CHECK(unint_pre_main_A0 == "Verdun (1,200)");
	CHECK(unint_pre_mini_A0 == "Verdun");
	CHECK(unint_pre_mini_A1 == "Verdun Fortress");
	CHECK(unint_pre_mini_B0 == "Metz");

	/* Continuation Step 1: Side 0 scouts Tile A -> discovers "Verdun Fortress", pop 3500, city flag true */
	MilitaryFog::RefreshStationarySnapshot(side0, tile_A);
	const TownSnapshot *unint_step1_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	REQUIRE(unint_step1_A0 != nullptr);
	std::string unint_step1_main_A0 = unint_step1_A0->GetViewportString(false);
	std::string unint_step1_mini_A0 = unint_step1_A0->GetViewportString(true);
	bool unint_step1_larger_A0 = unint_step1_A0->larger_town;
	CHECK(unint_step1_mini_A0 == "Verdun Fortress");
	CHECK(unint_step1_larger_A0);

	/* Continuation Step 2: Side 0 scouts Tile B -> discovers deletion of Town B */
	MilitaryFog::RefreshStationarySnapshot(side0, tile_B);
	CHECK(MilitaryFog::GetTownSnapshotAtTile(side0, tile_B) == nullptr);

	/* Continuation Step 3: Town C created reusing Town B's ID at Tile C, Side 0 scouts Tile C */
	REQUIRE(Town::CanAllocateItem());
	Town *town_C = Town::CreateAtIndex(id_B, tile_C);
	REQUIRE(town_C != nullptr);
	town_C->name = "Nancy";
	town_C->townnametype = SPECSTR_TOWNNAME_START;
	town_C->cache.population = 5600;
	town_C->larger_town = false;
	town_C->UpdateVirtCoord();

	MilitaryFog::RefreshStationarySnapshot(side0, tile_C);
	const TownSnapshot *unint_step3_C0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_C);
	REQUIRE(unint_step3_C0 != nullptr);
	std::string unint_step3_main_C0 = unint_step3_C0->GetViewportString(false);
	std::string unint_step3_mini_C0 = unint_step3_C0->GetViewportString(true);
	uint32_t unint_step3_handle_C0 = unint_step3_C0->handle;
	CHECK(unint_step3_main_C0 == "Nancy (5,600)");
	CHECK(unint_step3_mini_C0 == "Nancy");

	/* Clean up live objects on uninterrupted branch */
	delete town_A;
	delete town_C;

	/* --- PATH 2: Loaded continuation --- */
	MilitaryFog::Reset();
	REQUIRE_FALSE(MilitaryFog::IsActive());

	SaveLoadResult load_res = SaveOrLoad("test_town_labels_continuation.sav", SaveLoadOperation::Load, DetailedFileType::GameFile, Subdirectory::None, false);
	REQUIRE(load_res == SaveLoadResult::Ok);
	std::remove("test_town_labels_continuation.sav");

	REQUIRE(MilitaryFog::IsActive());

	/* Immediately after load: check stale values, side isolation, and consumer data */
	const TownSnapshot *loaded_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	const TownSnapshot *loaded_B0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_B);
	const TownSnapshot *loaded_A1 = MilitaryFog::GetTownSnapshotAtTile(side1, tile_A);
	REQUIRE(loaded_A0 != nullptr);
	REQUIRE(loaded_B0 != nullptr);
	REQUIRE(loaded_A1 != nullptr);

	/* Side 0 retained stale values */
	CHECK(loaded_A0->handle == handle_A0);
	CHECK(loaded_A0->original_id == TownID{0});
	CHECK(loaded_A0->name == "Verdun");
	CHECK(loaded_A0->population == 1200);
	CHECK_FALSE(loaded_A0->larger_town);
	CHECK(loaded_A0->GetViewportString(false) == unint_pre_main_A0);
	CHECK(loaded_A0->GetViewportString(true) == unint_pre_mini_A0);

	/* Side 1 retained refreshed values */
	CHECK(loaded_A1->name == "Verdun Fortress");
	CHECK(loaded_A1->population == 3500);
	CHECK(loaded_A1->larger_town);
	CHECK(loaded_A1->GetViewportString(false) == unint_pre_main_A1);
	CHECK(loaded_A1->GetViewportString(true) == unint_pre_mini_A1);

	/* Stale Town B retained by Side 0 */
	CHECK(loaded_B0->handle == handle_B0);
	CHECK(loaded_B0->original_id == id_B);
	CHECK(loaded_B0->name == "Metz");
	CHECK(loaded_B0->population == 800);
	CHECK(loaded_B0->GetViewportString(false) == unint_pre_main_B0);
	CHECK(loaded_B0->GetViewportString(true) == unint_pre_mini_B0);

	Town *loaded_town_A = nullptr;
	for (Town *t : Town::Iterate()) {
		if (t->xy == tile_A) {
			loaded_town_A = t;
			break;
		}
	}
	REQUIRE(loaded_town_A != nullptr);
	loaded_town_A->cache.population = 3500;
	MilitaryFog::RefreshStationarySnapshot(side0, tile_A);
	const TownSnapshot *loaded_step1_A0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_A);
	REQUIRE(loaded_step1_A0 != nullptr);
	CHECK(loaded_step1_A0->GetViewportString(false) == unint_step1_main_A0);
	CHECK(loaded_step1_A0->GetViewportString(true) == unint_step1_mini_A0);
	CHECK(loaded_step1_A0->sign.width_normal == loaded_A1->sign.width_normal);
	CHECK(loaded_step1_A0->sign.width_small == loaded_A1->sign.width_small);
	CHECK(loaded_step1_A0->sign.width_normal > 0);
	CHECK(loaded_step1_A0->sign.width_small > 0);
	CHECK(loaded_step1_A0->larger_town == unint_step1_larger_A0);

	/* Loaded continuation Step 2: Side 0 scouts Tile B -> discovers deletion */
	MilitaryFog::RefreshStationarySnapshot(side0, tile_B);
	CHECK(MilitaryFog::GetTownSnapshotAtTile(side0, tile_B) == nullptr);

	/* Loaded continuation Step 3: Town C created reusing Town B's ID at Tile C, Side 0 scouts Tile C */
	REQUIRE(Town::CanAllocateItem());
	Town *loaded_town_C = Town::CreateAtIndex(id_B, tile_C);
	REQUIRE(loaded_town_C != nullptr);
	loaded_town_C->name = "Nancy";
	loaded_town_C->townnametype = SPECSTR_TOWNNAME_START;
	loaded_town_C->cache.population = 5600;
	loaded_town_C->larger_town = false;
	loaded_town_C->UpdateVirtCoord();

	MilitaryFog::RefreshStationarySnapshot(side0, tile_C);
	const TownSnapshot *loaded_step3_C0 = MilitaryFog::GetTownSnapshotAtTile(side0, tile_C);
	REQUIRE(loaded_step3_C0 != nullptr);
	CHECK(loaded_step3_C0->GetViewportString(false) == unint_step3_main_C0);
	CHECK(loaded_step3_C0->GetViewportString(true) == unint_step3_mini_C0);
	CHECK(loaded_step3_C0->handle == unint_step3_handle_C0);

	/* Clean up loaded live objects */
	for (Town *t : Town::Iterate()) {
		delete t;
	}
	ResetFogTestEnvironment();
}
