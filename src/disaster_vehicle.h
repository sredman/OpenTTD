/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file disaster_vehicle.h All disaster vehicles. */

#ifndef DISASTER_VEHICLE_H
#define DISASTER_VEHICLE_H

#include "vehicle_base.h"
#include "aircraft.h"

/** Different sub types of disaster vehicles. */
enum DisasterSubType : uint8_t {
	ST_ZEPPELINER,               ///< Zeppelin, crashes at airports.
	ST_ZEPPELINER_SHADOW,        ///< Shadow of the zeppelin.
	ST_SMALL_UFO,                ///< Small UFO, tries to find a road vehicle to destroy.
	ST_SMALL_UFO_SHADOW,         ///< Shadow of small UFO
	ST_AIRPLANE,                 ///< Airplane destroying an oil refinery
	ST_AIRPLANE_SHADOW,          ///< Shadow of airplane
	ST_HELICOPTER,               ///< Helicopter destroying a factory.
	ST_HELICOPTER_SHADOW,        ///< Shadow of helicopter.
	ST_HELICOPTER_ROTORS,        ///< Rotors of helicopter.
	ST_BIG_UFO,                  ///< Big UFO, finds a piece of railroad to "park" on
	ST_BIG_UFO_SHADOW,           ///< Shadow of the big UFO
	ST_BIG_UFO_DESTROYER,        ///< Aircraft the will bomb the big UFO
	ST_BIG_UFO_DESTROYER_SHADOW, ///< Shadow of the aircraft.
	ST_SMALL_SUBMARINE,          ///< Small submarine, pops up in the oceans but doesn't do anything
	ST_BIG_SUBMARINE,            ///< Big submarine, pops up in the oceans but doesn't do anything
	ST_MILITARY_AIRCRAFT,        ///< Autonomous military aircraft.
	ST_MILITARY_AIRCRAFT_SHADOW, ///< Shadow of the military aircraft.
	ST_MILITARY_TARGET_MARKER,   ///< Fixed military target marker.
};

/** Military movement state for autonomous military units. */
enum MilitaryMovementState : uint8_t {
	MMS_IDLE = 0,    ///< Idle (no targets or waiting).
	MMS_MOVING = 1,  ///< Moving straight towards current target.
	MMS_ARRIVED = 2, ///< Arrived at target marker.
};

/**
 * Disasters, like submarines, skyrangers and their shadows, belong to this class.
 */
struct DisasterVehicle final : public SpecializedVehicle<DisasterVehicle, VehicleType::Disaster> {
	SpriteID image_override{}; ///< Override for the default disaster vehicle sprite.
	VehicleID big_ufo_destroyer_target = VehicleID::Invalid(); ///< The big UFO that this destroyer is supposed to bomb.
	VehicleAirFlags flags{}; ///< Flags about the state of the vehicle, @see VehicleAirFlags
	uint16_t state = 0; ///< Action stage of the disaster vehicle.

	/**
	 * For use by saveload.
	 * @param index The index within the vehicle pool.
	 */
	DisasterVehicle(VehicleID index) : SpecializedVehicleBase(index) {}
	DisasterVehicle(VehicleID index, int x, int y, Direction direction, DisasterSubType subtype, VehicleID big_ufo_destroyer_target = VehicleID::Invalid());
	/** We want to 'destruct' the right class. */
	~DisasterVehicle() override = default;

	void UpdatePosition(int x, int y, int z);
	void UpdateDeltaXY() override;
	void UpdateImage();
	bool Tick() override;

	uint8_t GetMilitarySide() const { return GB(this->state, 8, 8); }
	void SetMilitarySide(uint8_t side) { SB(this->state, 8, 8, side); }
	MilitaryMovementState GetMilitaryMovementState() const { return (MilitaryMovementState)GB(this->state, 0, 8); }
	void SetMilitaryMovementState(MilitaryMovementState mms) { SB(this->state, 0, 8, (uint8_t)mms); }
};

DisasterVehicle *SpawnMilitaryAircraft(int x, int y, Direction dir, uint8_t side);
DisasterVehicle *SpawnMilitaryTargetMarker(int x, int y, uint8_t side);
DisasterVehicle *FindNearestEligibleEnemyMarker(const DisasterVehicle *aircraft);
void SetupMilitaryScenario();
void MaybeSetupMilitaryScenario();

#endif /* DISASTER_VEHICLE_H */
