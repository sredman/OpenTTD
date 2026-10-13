class AircraftRegression extends GSController {
	function Start();
};

function AircraftRegression::Start()
{
	print("");
	print("--AircraftRegression--");
	print(" Ops:                 " + this.GetOpsTillSuspend());
	print(" InitialTick:         " + this.GetTick());
	this.Sleep(1);
	print(" StepTick:            " + this.GetTick());
	print(" PlaneSpeedValid:     " + GSGameSettings.IsValid("vehicle.plane_speed"));
	print(" PlaneSpeedValue:     " + GSGameSettings.GetValue("vehicle.plane_speed"));
	print(" CivilianBaseline:    OK");
	print("");
	print("Done...");
	print("");
}
