/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file military_aircraft.cpp Focused unit tests for autonomous military aircraft slice. */

#include "../stdafx.h"
#include "../3rdparty/catch2/catch.hpp"
#include "../disaster_vehicle.h"
#include "../map_func.h"
#include "../saveload/saveload_internal.h"
#include "mock_environment.h"

#include "../safeguards.h"

static void ResetTestEnvironment()
{
	MockEnvironment::Instance();
	_vehicle_pool.CleanPool();
	Map::Allocate(256, 256);
}

TEST_CASE("MilitaryAircraft - Modeling, side affiliation, and civilian separation")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	REQUIRE(plane != nullptr);
	CHECK(plane->type == VehicleType::Disaster);
	CHECK(plane->subtype == ST_MILITARY_AIRCRAFT);
	CHECK(plane->owner == OWNER_NONE);
	CHECK(plane->GetMilitarySide() == 0);
	CHECK(plane->GetMilitaryMovementState() == MMS_IDLE);

	DisasterVehicle *shadow = plane->Next();
	REQUIRE(shadow != nullptr);
	CHECK(shadow->subtype == ST_MILITARY_AIRCRAFT_SHADOW);
	CHECK(shadow->vehstatus.Test(VehState::Shadow));

	DisasterVehicle *marker = SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(marker != nullptr);
	CHECK(marker->type == VehicleType::Disaster);
	CHECK(marker->subtype == ST_MILITARY_TARGET_MARKER);
	CHECK(marker->owner == OWNER_NONE);
	CHECK(marker->GetMilitarySide() == 1);
}

TEST_CASE("MilitaryAircraft - Straight line movement for off-axis targets")
{
	ResetTestEnvironment();

	/* Aircraft on Side 0 at (100, 100). */
	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::SW, 0);
	REQUIRE(plane != nullptr);

	/* Enemy target marker with unequal axis offset: +80 pixels in X, +20 pixels in Y. */
	DisasterVehicle *enemy = SpawnMilitaryTargetMarker(100 * TILE_SIZE + 80, 100 * TILE_SIZE + 20, 1);
	REQUIRE(enemy != nullptr);

	int initial_x = plane->x_pos;
	int initial_y = plane->y_pos;

	/* First tick: aircraft targets enemy marker, enters MMS_MOVING, and advances. */
	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == enemy->index);
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);

	/* Run up to tick 20: along straight line segment (+80, +20), when dx = 20, dy = 5. */
	for (int i = 1; i < 20; i++) {
		plane->Tick();
	}
	CHECK(plane->x_pos == initial_x + 20);
	CHECK(plane->y_pos == initial_y + 5);

	/* Run up to tick 40: when dx = 40, dy = 10. */
	for (int i = 20; i < 40; i++) {
		plane->Tick();
	}
	CHECK(plane->x_pos == initial_x + 40);
	CHECK(plane->y_pos == initial_y + 10);

	/* Run up to tick 60: when dx = 60, dy = 15. */
	for (int i = 40; i < 60; i++) {
		plane->Tick();
	}
	CHECK(plane->x_pos == initial_x + 60);
	CHECK(plane->y_pos == initial_y + 15);

	/* Continue ticking until arrival. */
	for (int i = 60; i < 85; i++) {
		plane->Tick();
	}
	CHECK(plane->GetMilitaryMovementState() == MMS_ARRIVED);

	/* Equivalent continuation: additional ticks stay arrived without diverging. */
	int arrive_x = plane->x_pos;
	int arrive_y = plane->y_pos;
	for (int i = 0; i < 20; i++) {
		plane->Tick();
	}
	CHECK(plane->GetMilitaryMovementState() == MMS_ARRIVED);
	CHECK(plane->x_pos == arrive_x);
	CHECK(plane->y_pos == arrive_y);
}

TEST_CASE("MilitaryAircraft - Deterministic tie-breaking between equidistant enemy markers")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::N, 0);
	REQUIRE(plane != nullptr);

	/* Two enemy markers at identical distance (20 tiles) from aircraft. */
	DisasterVehicle *enemyA = SpawnMilitaryTargetMarker(100 * TILE_SIZE, 80 * TILE_SIZE, 1);
	DisasterVehicle *enemyB = SpawnMilitaryTargetMarker(100 * TILE_SIZE, 120 * TILE_SIZE, 1);
	REQUIRE(enemyA != nullptr);
	REQUIRE(enemyB != nullptr);

	VehicleID expected_target = std::min(enemyA->index, enemyB->index);

	DisasterVehicle *best = FindNearestEligibleEnemyMarker(plane);
	REQUIRE(best != nullptr);
	CHECK(best->index == expected_target);

	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == expected_target);
}

TEST_CASE("MilitaryAircraft - Bounded handling of no targets (idle)")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	REQUIRE(plane != nullptr);

	/* Only friendly markers exist on map. */
	SpawnMilitaryTargetMarker(80 * TILE_SIZE, 100 * TILE_SIZE, 0);
	SpawnMilitaryTargetMarker(120 * TILE_SIZE, 100 * TILE_SIZE, 0);

	int start_x = plane->x_pos;
	int start_y = plane->y_pos;

	plane->Tick();
	CHECK(plane->GetMilitaryMovementState() == MMS_IDLE);
	CHECK(plane->big_ufo_destroyer_target == VehicleID::Invalid());
	CHECK(plane->x_pos == start_x);
	CHECK(plane->y_pos == start_y);
}

TEST_CASE("MilitaryAircraft - Bounded handling of arrival at target marker")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	REQUIRE(plane != nullptr);

	/* Place target marker within arrival distance (4 pixels away). */
	DisasterVehicle *enemy = SpawnMilitaryTargetMarker(100 * TILE_SIZE + 4, 100 * TILE_SIZE, 1);
	REQUIRE(enemy != nullptr);

	plane->Tick();
	CHECK(plane->GetMilitaryMovementState() == MMS_ARRIVED);
	CHECK(plane->big_ufo_destroyer_target == enemy->index);

	/* Additional ticks keep the aircraft in arrived state without runaway movement. */
	int arrive_x = plane->x_pos;
	int arrive_y = plane->y_pos;
	for (int i = 0; i < 10; i++) {
		plane->Tick();
	}
	CHECK(plane->GetMilitaryMovementState() == MMS_ARRIVED);
	CHECK(plane->x_pos == arrive_x);
	CHECK(plane->y_pos == arrive_y);
}

TEST_CASE("MilitaryAircraft - Bounded handling of map edges")
{
	ResetTestEnvironment();

	/* Meaningful moving boundary case: aircraft placed at boundary x = 0. */
	DisasterVehicle *plane = SpawnMilitaryAircraft(0, 50 * TILE_SIZE, Direction::S, 0);
	REQUIRE(plane != nullptr);

	/* Target marker along boundary at (0, 100 * TILE_SIZE) (50 tiles away, dist > 8). */
	DisasterVehicle *enemy = SpawnMilitaryTargetMarker(0, 100 * TILE_SIZE, 1);
	REQUIRE(enemy != nullptr);

	int initial_y = plane->y_pos;

	/* First tick: acquires target, moves along boundary. */
	plane->Tick();
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);
	CHECK(plane->x_pos == 0);
	CHECK(plane->y_pos > initial_y);

	/* Advance 30 ticks along the boundary. */
	for (int i = 0; i < 30; i++) {
		plane->Tick();
	}

	CHECK(plane->x_pos == 0);
	CHECK(plane->y_pos == initial_y + 31);
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);
	CHECK(plane->x_pos <= (int)(Map::MaxX() * TILE_SIZE));
	CHECK(plane->y_pos <= (int)(Map::MaxY() * TILE_SIZE));

	/* Upper boundary test: aircraft at Map::MaxX() * TILE_SIZE moving along boundary. */
	int max_x = Map::MaxX() * TILE_SIZE;
	DisasterVehicle *plane_max = SpawnMilitaryAircraft(max_x, 50 * TILE_SIZE, Direction::S, 0);
	REQUIRE(plane_max != nullptr);

	DisasterVehicle *enemy_max = SpawnMilitaryTargetMarker(max_x, 100 * TILE_SIZE, 1);
	REQUIRE(enemy_max != nullptr);

	int max_initial_y = plane_max->y_pos;
	plane_max->Tick();
	CHECK(plane_max->GetMilitaryMovementState() == MMS_MOVING);
	CHECK(plane_max->x_pos == max_x);
	CHECK(plane_max->y_pos > max_initial_y);

	for (int i = 0; i < 30; i++) {
		plane_max->Tick();
	}
	CHECK(plane_max->x_pos == max_x);
	CHECK(plane_max->y_pos == max_initial_y + 31);
	CHECK(plane_max->GetMilitaryMovementState() == MMS_MOVING);
	CHECK(plane_max->y_pos <= (int)(Map::MaxY() * TILE_SIZE));
}

TEST_CASE("MilitaryAircraft - Bounded handling of removed targets")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	REQUIRE(plane != nullptr);

	DisasterVehicle *enemy1 = SpawnMilitaryTargetMarker(120 * TILE_SIZE, 100 * TILE_SIZE, 1);
	DisasterVehicle *enemy2 = SpawnMilitaryTargetMarker(150 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(enemy1 != nullptr);
	REQUIRE(enemy2 != nullptr);

	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == enemy1->index);
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);

	/* Remove current target enemy1. */
	VehicleID enemy2_id = enemy2->index;
	delete enemy1;

	/* Next tick: detects target is gone, retargets to enemy2. */
	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == enemy2_id);
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);

	/* Remove enemy2: no targets left. */
	delete enemy2;
	plane->Tick();
	CHECK(plane->big_ufo_destroyer_target == VehicleID::Invalid());
	CHECK(plane->GetMilitaryMovementState() == MMS_IDLE);
}

TEST_CASE("MilitaryAircraft - Licensed placeholders and side identity palette")
{
	ResetTestEnvironment();

	DisasterVehicle *plane0 = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	DisasterVehicle *plane1 = SpawnMilitaryAircraft(120 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 1);
	DisasterVehicle *marker0 = SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 0);
	DisasterVehicle *marker1 = SpawnMilitaryTargetMarker(160 * TILE_SIZE, 100 * TILE_SIZE, 1);

	REQUIRE(plane0 != nullptr);
	REQUIRE(plane1 != nullptr);
	REQUIRE(marker0 != nullptr);
	REQUIRE(marker1 != nullptr);

	/* Sprites match licensed placeholders. */
	CHECK(plane0->sprite_cache.sprite_seq.seq[0].sprite == SPR_F_15);
	CHECK(plane1->sprite_cache.sprite_seq.seq[0].sprite == SPR_F_15);
	CHECK(marker0->sprite_cache.sprite_seq.seq[0].sprite == SPR_TRANSMITTER);
	CHECK(marker1->sprite_cache.sprite_seq.seq[0].sprite == SPR_TRANSMITTER);

	/* Side identity recolour palettes. */
	CHECK(plane0->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_BLUE);
	CHECK(plane1->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_RED);
	CHECK(marker0->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_BLUE);
	CHECK(marker1->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_RED);
}

TEST_CASE("MilitaryAircraft - Deterministic simulation & save/load persistence")
{
	ResetTestEnvironment();

	/* Run 1: 50 ticks of simulation from initial setup. */
	DisasterVehicle *plane1 = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(plane1 != nullptr);

	for (int i = 0; i < 50; i++) {
		plane1->Tick();
	}

	int r1_x = plane1->x_pos;
	int r1_y = plane1->y_pos;
	Direction r1_dir = plane1->direction;
	uint16_t r1_state = plane1->state;
	VehicleID r1_target = plane1->big_ufo_destroyer_target;

	/* Run 2: Exact identical setup repeated. */
	ResetTestEnvironment();

	DisasterVehicle *plane2 = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::E, 0);
	SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(plane2 != nullptr);

	for (int i = 0; i < 50; i++) {
		plane2->Tick();
	}

	CHECK(plane2->x_pos == r1_x);
	CHECK(plane2->y_pos == r1_y);
	CHECK(plane2->direction == r1_dir);
	CHECK(plane2->state == r1_state);
	CHECK(plane2->big_ufo_destroyer_target == r1_target);

	/* Run 3: Continued simulation for another 50 ticks (total 100 ticks). */
	for (int i = 0; i < 50; i++) {
		plane2->Tick();
	}
	int total_100_x = plane2->x_pos;
	int total_100_y = plane2->y_pos;

	/* Persistence check: Re-initialize with saved state from tick 50 and run 50 ticks. */
	ResetTestEnvironment();

	DisasterVehicle *loaded_plane = SpawnMilitaryAircraft(r1_x, r1_y, r1_dir, 0);
	SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(loaded_plane != nullptr);
	loaded_plane->state = r1_state;
	loaded_plane->big_ufo_destroyer_target = r1_target;

	for (int i = 0; i < 50; i++) {
		loaded_plane->Tick();
	}

	CHECK(loaded_plane->x_pos == total_100_x);
	CHECK(loaded_plane->y_pos == total_100_y);
}

TEST_CASE("MilitaryAircraft - Restore displayed side identity on load and persistence")
{
	ResetTestEnvironment();

	DisasterVehicle *plane = SpawnMilitaryAircraft(100 * TILE_SIZE, 100 * TILE_SIZE, Direction::SW, 0);
	DisasterVehicle *marker = SpawnMilitaryTargetMarker(140 * TILE_SIZE, 100 * TILE_SIZE, 1);
	REQUIRE(plane != nullptr);
	REQUIRE(marker != nullptr);

	/* Verify initial side recolouring. */
	CHECK(plane->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_BLUE);
	CHECK(marker->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_RED);

	for (int i = 0; i < 10; i++) {
		plane->Tick();
	}
	int pre_load_x = plane->x_pos;

	/* Simulate save/load round-trip: SlVehicleDisaster does not persist sprite_cache palette.
	 * Clear palette to mimic raw deserialization before post-load processing. */
	plane->sprite_cache.sprite_seq.seq[0].pal = 0;
	marker->sprite_cache.sprite_seq.seq[0].pal = 0;

	/* Run AfterLoadVehiclesPhase2 (the post-load vehicle reconstruction hook). */
	AfterLoadVehiclesPhase2(true);

	/* Verify side identity palettes are correctly restored on load. */
	CHECK(plane->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_BLUE);
	CHECK(marker->sprite_cache.sprite_seq.seq[0].pal == PALETTE_TO_RED);

	/* Verify movement continuation resumes seamlessly after load. */
	for (int i = 0; i < 10; i++) {
		plane->Tick();
	}
	CHECK(plane->x_pos > pre_load_x);
	CHECK(plane->GetMilitaryMovementState() == MMS_MOVING);
}

TEST_CASE("MilitaryAircraft - Scenario setup fixture")
{
	ResetTestEnvironment();

	bool ok = SetupMilitaryScenario();
	CHECK(ok);

	int plane_count = 0;
	int marker_count = 0;
	int side0_markers = 0;
	int side1_markers = 0;
	DisasterVehicle *aircraft = nullptr;

	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT) {
			plane_count++;
			aircraft = v;
		} else if (v->subtype == ST_MILITARY_TARGET_MARKER) {
			marker_count++;
			if (v->GetMilitarySide() == 0) side0_markers++;
			if (v->GetMilitarySide() == 1) side1_markers++;
		}
	}

	CHECK(plane_count == 1);
	CHECK(marker_count == 3);
	CHECK(side0_markers == 1);
	CHECK(side1_markers == 2);
	REQUIRE(aircraft != nullptr);
	CHECK(aircraft->GetMilitarySide() == 0);

	/* Aircraft targets nearest eligible enemy marker and moves toward it. */
	aircraft->Tick();
	CHECK(aircraft->GetMilitaryMovementState() == MMS_MOVING);
	DisasterVehicle *target = DisasterVehicle::GetIfValid(aircraft->big_ufo_destroyer_target);
	REQUIRE(target != nullptr);
	CHECK(target->GetMilitarySide() == 1);
	CHECK(target->x_pos == 140 * TILE_SIZE);
	CHECK(target->y_pos == 100 * TILE_SIZE);
}

TEST_CASE("MilitaryAircraft - Ordinary load preservation and small map refusal")
{
	ResetTestEnvironment();

	/* Verify no military vehicles exist initially in an ordinary clean environment. */
	int military_count = 0;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT || v->subtype == ST_MILITARY_TARGET_MARKER) military_count++;
	}
	CHECK(military_count == 0);

	/* Small map: 128x128. SetupMilitaryScenario must refuse and leave world untouched. */
	Map::Allocate(128, 128);
	bool setup_ok = SetupMilitaryScenario();
	CHECK(!setup_ok);

	military_count = 0;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT || v->subtype == ST_MILITARY_TARGET_MARKER) military_count++;
	}
	CHECK(military_count == 0);

	/* 64x64 map: must also refuse before changing the world. */
	Map::Allocate(64, 64);
	setup_ok = SetupMilitaryScenario();
	CHECK(!setup_ok);

	military_count = 0;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT || v->subtype == ST_MILITARY_TARGET_MARKER) military_count++;
	}
	CHECK(military_count == 0);

	/* 256x256 map: accepts and initializes scenario. */
	Map::Allocate(256, 256);
	setup_ok = SetupMilitaryScenario();
	CHECK(setup_ok);

	military_count = 0;
	for (DisasterVehicle *v : DisasterVehicle::Iterate()) {
		if (v->subtype == ST_MILITARY_AIRCRAFT || v->subtype == ST_MILITARY_TARGET_MARKER) military_count++;
	}
	CHECK(military_count > 0);
}
