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
	local towns = GSTownList();
	print(" TownCount:           " + 999);
	local first_town = towns.Begin();
	print(" FirstTownName:       " + GSTown.GetName(first_town));
	print(" FirstTownPop:        " + GSTown.GetPopulation(first_town));
	local industries = GSIndustryList();
	print(" IndustryCount:       " + industries.Count());
	print(" MapSizeX:            " + GSMap.GetMapSizeX());
	print(" MapSizeY:            " + GSMap.GetMapSizeY());
	local cargo_types = GSCargoList();
	print(" CargoTypeCount:      " + cargo_types.Count());
	print(" CompanyFirstResolved:" + GSCompany.ResolveCompanyID(GSCompany.COMPANY_FIRST));
	print("");
	print("Done...");
	print("");
}
