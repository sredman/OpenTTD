class AircraftRegression extends GSInfo {
	function GetAuthor()      { return "Agent Rig"; }
	function GetName()        { return "AircraftRegression"; }
	function GetShortName()   { return "ACRG"; }
	function GetDescription() { return "Scenario test entry point for aircraft and military feature increments."; }
	function GetVersion()     { return 1; }
	function GetAPIVersion()  { return "16"; }
	function GetDate()        { return "2026-10-13"; }
	function CreateInstance() { return "AircraftRegression"; }
	function UseAsRandomAI()  { return false; }
}

RegisterGS(AircraftRegression());
