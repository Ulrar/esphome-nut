/*
 * ESPHome port of drivers/libhid.c helper functions and the usage
 * lookup tables from drivers/libhid.c and drivers/mge-hid.c.
 *
 * Copyright (C) 2003-2007 Arnaud Quette, Peter Selinger,
 * Charles Lepple, Arjen de Korte, MGE UPS SYSTEMS, and the NUT team.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "nut_libhid.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

#include "nut_shim_common.h"

#define SMALLBUF 128
#ifndef upsdebugx
# define upsdebugx(...) ((void)0)
#endif
#ifndef PRIuMAX
# define PRIuMAX "llu"
#endif

/* Units and exponents table (HID PDC, 3.2.3), verbatim from libhid.c */
#define NB_HID_UNITS 10
static const struct {
	const long	Type;
	const int8_t	Expo;
} HIDUnits[NB_HID_UNITS] = {
	{ 0x00000000, 0 },	/* None */
	{ 0x00F0D121, 7 },	/* Voltage */
	{ 0x00100001, 0 },	/* Ampere */
	{ 0x0000D121, 7 },	/* VA */
	{ 0x0000D121, 7 },	/* Watts */
	{ 0x00001001, 0 },	/* second */
	{ 0x00010001, 0 },	/* K */
	{ 0x00000000, 0 },	/* percent */
	{ 0x0000F001, 0 },	/* Hertz */
	{ 0x00101001, 0 },	/* As */
};

static const char *hid_lookup_path(const HIDNode_t usage, usage_tables_t *utab);
static long hid_lookup_usage(const char *name, usage_tables_t *utab);

static int snprintfcat(char *dst, size_t size, const char *fmt, ...)
{
	va_list ap;
	size_t used = strlen(dst);
	int ret;

	if (used >= size) {
		return -1;
	}

	va_start(ap, fmt);
	ret = vsnprintf(dst + used, size - used, fmt, ap);
	va_end(ap);
	return ret;
}

double logical_to_physical(HIDData_t *Data, long logical)
{
	double physical;
	double Factor;

	upsdebugx(5, "PhyMax = %ld, PhyMin = %ld, LogMax = %ld, LogMin = %ld",
		Data->PhyMax, Data->PhyMin, Data->LogMax, Data->LogMin);

	/* HID spec says that if one or both are undefined, or if they are
	 * both 0, then PhyMin = LogMin, PhyMax = LogMax. */
	if (!Data->have_PhyMax || !Data->have_PhyMin ||
		(Data->PhyMax == 0 && Data->PhyMin == 0))
	{
		return (double)logical;
	}

	/* Paranoia */
	if ((Data->PhyMax <= Data->PhyMin) || (Data->LogMax <= Data->LogMin))
	{
		/* this should not really happen */
		upsdebugx(5, "Max was not greater than Min, returning logical value as is");
		return (double)logical;
	}

	Factor = (double)(Data->PhyMax - Data->PhyMin) / (Data->LogMax - Data->LogMin);
	/* Convert Value */
	physical = (double)((logical - Data->LogMin) * Factor) + Data->PhyMin;

	if (physical > Data->PhyMax) {
		return Data->PhyMax;
	}

	if (physical < Data->PhyMin) {
		return Data->PhyMin;
	}

	return physical;
}

static int8_t get_unit_expo(const HIDData_t *hiddata)
{
	int	i;
	int8_t	unit_expo = hiddata->UnitExp;

	upsdebugx(5, "Unit = %08x, UnitExp = %d", (uint32_t)(hiddata->Unit), hiddata->UnitExp);

	for (i = 0; i < NB_HID_UNITS; i++) {

		if (HIDUnits[i].Type == hiddata->Unit) {
			unit_expo -= HIDUnits[i].Expo;
			break;
		}
	}

	upsdebugx(5, "Exponent = %d", unit_expo);
	return unit_expo;
}

static double exponent(double a, int8_t b)
{
	if (b>0)
		return (a * exponent(a, --b));		/* a * a ... */

	if (b<0)
		return ((1/a) * exponent(a, ++b));	/* (1/a) * (1/a) ... */

	return 1;
}

int string_to_path(const char *string, HIDPath_t *path, usage_tables_t *utab)
{
	int	i = 0;
	long	usage;
	char	buf[SMALLBUF];
	char	*token, *last;

	snprintf(buf, sizeof(buf), "%s", string);

	for (token = strtok_r(buf, ".", &last); token != NULL; token = strtok_r(NULL, ".", &last))
	{
		/* lookup tables first (to override defaults) */
		/* Note/FIXME?: we happen to process "usage" as a "signed long"
		 * while the HIDNode_t behind it is (currently) uint32_t.
		 * The method below returns `-1` for entries not found; however
		 * half of our permissible range may seem negative and be valid.
		 */
		if ((usage = hid_lookup_usage(token, utab)) != -1)
		{
			path->Node[i++] = (HIDNode_t)usage;
			continue;
		} else {
			/* hid_lookup_usage() logs for itself: ... -> not found in lookup table */
			upsdebugx(5, "string_to_path: hid_lookup_usage failed, "
				"checking if token %s is a raw value", token);
		}

		/* translate unnamed path components such as "ff860024" */
		if (strlen(token) == strspn(token, "1234567890abcdefABCDEF"))
		{
			long l = strtol(token, NULL, 16);
			/* Note: currently per hidtypes.h, HIDNode_t == uint32_t */
			if (l < 0 || (uintmax_t)l > (uintmax_t)UINT32_MAX) {
				upsdebugx(5, "string_to_path: badvalue (pathcomp): "
					"%ld negative or %" PRIuMAX " too large",
					l, (uintmax_t)l);
				goto badvalue;
			}
			path->Node[i++] = (HIDNode_t)l;
			continue;
		}

		/* indexed collection */
		if (strlen(token) == strspn(token, "[1234567890]"))
		{
			int l = atoi(token + 1); /* +1: skip the bracket */
			if (l < 0 || (uintmax_t)l > (uintmax_t)UINT32_MAX) {
				upsdebugx(5, "string_to_path: badvalue(indexed): "
					"%d negative or %" PRIuMAX " too large",
					l, (uintmax_t)l);
				goto badvalue;
			}
			path->Node[i++] = 0x00ff0000 + (HIDNode_t)l;
			continue;
		}

badvalue:
		/* Uh oh, typo in usage table? */
		upsdebugx(1, "string_to_path: couldn't parse %s from %s", token, string);
	}

	if (i < 0 || i > (int)UINT8_MAX) {
		fatalx(EXIT_FAILURE, "Error: string_to_path(): length exceeded");
	}
	path->Size = (uint8_t)i; /* by construct, i>=0; but anyway checked above to be sure */

	upsdebugx(4, "string_to_path: depth = %d", path->Size);
	return i;
}

int path_to_string(char *string, size_t size, const HIDPath_t *path, usage_tables_t *utab)
{
	int	i;
	const char	*p;

	snprintf(string, size, "%s", "");

	for (i = 0; i < path->Size; i++)
	{
		if (i > 0)
			snprintfcat(string, size, ".");

		/* lookup tables first (to override defaults) */
		if (utab != NULL && ((p = hid_lookup_path(path->Node[i], utab)) != NULL))
		{
			snprintfcat(string, size, "%s", p);
			continue;
		}

		/* indexed collection */
		if ((path->Node[i] & 0xffff0000) == 0x00ff0000)
		{
			snprintfcat(string, size, "[%u]", path->Node[i] & 0x0000ffff);
			continue;
		}

		/* unnamed path components such as "ff860024" */
		snprintfcat(string, size, "%08x", path->Node[i]);
	}

	return i;
}

static long hid_lookup_usage(const char *name, usage_tables_t *utab)
{
	int i, j;

	for (i = 0; utab[i] != NULL; i++)
	{
		for (j = 0; utab[i][j].usage_name != NULL; j++)
		{
			if (strcasecmp(utab[i][j].usage_name, name))
				continue;

			/* Note: currently per hidtypes.h, HIDNode_t == uint32_t */
			upsdebugx(5, "hid_lookup_usage: %s -> %08x", name, (uint32_t)utab[i][j].usage_code);
			return (long)(utab[i][j].usage_code);
		}
	}

	upsdebugx(5, "hid_lookup_usage: %s -> not found in lookup table", name);
	return -1;
}

static const char *hid_lookup_path(const HIDNode_t usage, usage_tables_t *utab)
{
	int i, j;

	for (i = 0; utab[i] != NULL; i++)
	{
		for (j = 0; utab[i][j].usage_name != NULL; j++)
		{
			if (utab[i][j].usage_code != usage)
				continue;

			upsdebugx(5, "hid_lookup_path: %08x -> %s", (unsigned int)usage, utab[i][j].usage_name);
			return utab[i][j].usage_name;
		}
	}

	upsdebugx(5, "hid_lookup_path: %08x -> not found in lookup table", (unsigned int)usage);
	return NULL;
}

usage_lkp_t nut_hid_usage_lkp[] = {
	/* Power Device Page */
	{  "Undefined",				0x00840000 },
	{  "iName",				0x00840001 },
	{  "PresentStatus",			0x00840002 },
	{  "ChangedStatus",			0x00840003 },
	{  "UPS",				0x00840004 },
	{  "PowerSupply",			0x00840005 },
	/* 0x00840006-0x0084000f	=>	Reserved */
	{  "BatterySystem",			0x00840010 },
	{  "BatterySystemID",			0x00840011 },
	{  "Battery",				0x00840012 },
	{  "BatteryID",				0x00840013 },
	{  "Charger",				0x00840014 },
	{  "ChargerID",				0x00840015 },
	{  "PowerConverter",			0x00840016 },
	{  "PowerConverterID",			0X00840017 },
	{  "OutletSystem",			0x00840018 },
	{  "OutletSystemID",			0x00840019 },
	{  "Input",				0x0084001a },
	{  "InputID",				0x0084001b },
	{  "Output",				0x0084001c },
	{  "OutputID",				0x0084001d },
	{  "Flow",				0x0084001e },
	{  "FlowID",				0x0084001f },
	{  "Outlet",				0x00840020 },
	{  "OutletID",				0x00840021 },
	{  "Gang",				0x00840022 },
	{  "GangID",				0x00840023 },
	{  "PowerSummary",			0x00840024 },
	{  "PowerSummaryID",			0x00840025 },
	/* 0x00840026-0x0084002f	=>	Reserved */
	{  "Voltage",				0x00840030 },
	{  "Current",				0x00840031 },
	{  "Frequency",				0x00840032 },
	{  "ApparentPower",			0x00840033 },
	{  "ActivePower",			0x00840034 },
	{  "PercentLoad",			0x00840035 },
	{  "Temperature",			0x00840036 },
	{  "Humidity",				0x00840037 },
	{  "BadCount",				0x00840038 },
	/* 0x00840039-0x0084003f	=>	Reserved */
	{  "ConfigVoltage",			0x00840040 },
	{  "ConfigCurrent",			0x00840041 },
	{  "ConfigFrequency",			0x00840042 },
	{  "ConfigApparentPower",		0x00840043 },
	{  "ConfigActivePower",			0x00840044 },
	{  "ConfigPercentLoad",			0x00840045 },
	{  "ConfigTemperature",			0x00840046 },
	{  "ConfigHumidity",			0x00840047 },
	/* 0x00840048-0x0084004f	=>	Reserved */
	{  "SwitchOnControl",			0x00840050 },
	{  "SwitchOffControl",			0x00840051 },
	{  "ToggleControl",			0x00840052 },
	{  "LowVoltageTransfer",		0x00840053 },
	{  "HighVoltageTransfer",		0x00840054 },
	{  "DelayBeforeReboot",			0x00840055 },
	{  "DelayBeforeStartup",		0x00840056 },
	{  "DelayBeforeShutdown",		0x00840057 },
	{  "Test",				0x00840058 },
	{  "ModuleReset",			0x00840059 },
	{  "AudibleAlarmControl",		0x0084005a },
	/* 0x0084005b-0x0084005f	=>	Reserved */
	{  "Present",				0x00840060 },
	{  "Good",				0x00840061 },
	{  "InternalFailure",			0x00840062 },
	{  "VoltageOutOfRange",			0x00840063 },
	{  "FrequencyOutOfRange",		0x00840064 },
	{  "Overload",				0x00840065 },
	/* Note: the correct spelling is "Overload", not "OverLoad",
	 * according to the official specification, "Universal Serial
	 * Bus Usage Tables for HID Power Devices", Release 1.0,
	 * November 1, 1997 */
	{  "OverCharged",			0x00840066 },
	{  "OverTemperature", 			0x00840067 },
	{  "ShutdownRequested",			0x00840068 },
	{  "ShutdownImminent",			0x00840069 },
	{  "SwitchOn/Off",			0x0084006b },
	{  "Switchable",			0x0084006c },
	{  "Used",				0x0084006d },
	{  "Boost",				0x0084006e },
	{  "Buck",				0x0084006f },
	{  "Initialized",			0x00840070 },
	{  "Tested",				0x00840071 },
	{  "AwaitingPower",			0x00840072 },
	{  "CommunicationLost",			0x00840073 },
	/* 0x00840074-0x008400fc	=>	Reserved */
	{  "iManufacturer",			0x008400fd },
	{  "iProduct",				0x008400fe },
	{  "iSerialNumber",			0x008400ff },

	/* Battery System Page */
	{ "Undefined",				0x00850000 },
	{ "SMBBatteryMode",			0x00850001 },
	{ "SMBBatteryStatus",			0x00850002 },
	{ "SMBAlarmWarning",			0x00850003 },
	{ "SMBChargerMode",			0x00850004 },
	{ "SMBChargerStatus",			0x00850005 },
	{ "SMBChargerSpecInfo",			0x00850006 },
	{ "SMBSelectorState",			0x00850007 },
	{ "SMBSelectorPresets",			0x00850008 },
	{ "SMBSelectorInfo",			0x00850009 },
	/* 0x0085000A-0x0085000f	=>	Reserved */
	{ "OptionalMfgFunction1",		0x00850010 },
	{ "OptionalMfgFunction2",		0x00850011 },
	{ "OptionalMfgFunction3",		0x00850012 },
	{ "OptionalMfgFunction4",		0x00850013 },
	{ "OptionalMfgFunction5",		0x00850014 },
	{ "ConnectionToSMBus",			0x00850015 },
	{ "OutputConnection",			0x00850016 },
	{ "ChargerConnection",			0x00850017 },
	{ "BatteryInsertion",			0x00850018 },
	{ "Usenext",				0x00850019 },
	{ "OKToUse",				0x0085001a },
	{ "BatterySupported",			0x0085001b },
	{ "SelectorRevision",			0x0085001c },
	{ "ChargingIndicator",			0x0085001d },
	/* 0x0085001e-0x00850027	=>	Reserved */
	{ "ManufacturerAccess",			0x00850028 },
	{ "RemainingCapacityLimit",		0x00850029 },
	{ "RemainingTimeLimit",			0x0085002a },
	{ "AtRate",				0x0085002b },
	{ "CapacityMode",			0x0085002c },
	{ "BroadcastToCharger",			0x0085002d },
	{ "PrimaryBattery",			0x0085002e },
	{ "ChargeController",			0x0085002f },
	/* 0x00850030-0x0085003f	=>	Reserved */
	{ "TerminateCharge",			0x00850040 },
	{ "TerminateDischarge",			0x00850041 },
	{ "BelowRemainingCapacityLimit",	0x00850042 },
	{ "RemainingTimeLimitExpired",		0x00850043 },
	{ "Charging",				0x00850044 },
	{ "Discharging",			0x00850045 },
	{ "FullyCharged",			0x00850046 },
	{ "FullyDischarged",			0x00850047 },
	{ "ConditioningFlag",			0x00850048 },
	{ "AtRateOK",				0x00850049 },
	{ "SMBErrorCode",			0x0085004a },
	{ "NeedReplacement",			0x0085004b },
	/* 0x0085004c-0x0085005f	=>	Reserved */
	{ "AtRateTimeToFull",			0x00850060 },
	{ "AtRateTimeToEmpty",			0x00850061 },
	{ "AverageCurrent",			0x00850062 },
	{ "Maxerror",				0x00850063 },
	{ "RelativeStateOfCharge",		0x00850064 },
	{ "AbsoluteStateOfCharge",		0x00850065 },
	{ "RemainingCapacity",			0x00850066 },
	{ "FullChargeCapacity",			0x00850067 },
	{ "RunTimeToEmpty",			0x00850068 },
	{ "AverageTimeToEmpty",			0x00850069 },
	{ "AverageTimeToFull",			0x0085006a },
	{ "CycleCount",				0x0085006b },
	/* 0x0085006c-0x0085007f	=>	Reserved */
	{ "BattPackModelLevel",			0x00850080 },
	{ "InternalChargeController",		0x00850081 },
	{ "PrimaryBatterySupport",		0x00850082 },
	{ "DesignCapacity",			0x00850083 },
	{ "SpecificationInfo",			0x00850084 },
	{ "ManufacturerDate",			0x00850085 },
	{ "SerialNumber",			0x00850086 },
	{ "iManufacturerName",			0x00850087 },
	{ "iDevicename",			0x00850088 }, /* sic! */
	{ "iDeviceChemistry",			0x00850089 }, /* misspelled as "iDeviceChemistery" in spec. */
	{ "ManufacturerData",			0x0085008a },
	{ "Rechargeable",			0x0085008b },
	{ "WarningCapacityLimit",		0x0085008c },
	{ "CapacityGranularity1",		0x0085008d },
	{ "CapacityGranularity2",		0x0085008e },
	{ "iOEMInformation",			0x0085008f },
	/* 0x00850090-0x008500bf	=>	Reserved */
	{ "InhibitCharge",			0x008500c0 },
	{ "EnablePolling",			0x008500c1 },
	{ "ResetToZero",			0x008500c2 },
	/* 0x008500c3-0x008500cf	=>	Reserved */
	{ "ACPresent",				0x008500d0 },
	{ "BatteryPresent",			0x008500d1 },
	{ "PowerFail",				0x008500d2 },
	{ "AlarmInhibited",			0x008500d3 },
	{ "ThermistorUnderRange",		0x008500d4 },
	{ "ThermistorHot",			0x008500d5 },
	{ "ThermistorCold",			0x008500d6 },
	{ "ThermistorOverRange",		0x008500d7 },
	{ "VoltageOutOfRange",			0x008500d8 },
	{ "CurrentOutOfRange",			0x008500d9 },
	{ "CurrentNotRegulated",		0x008500da },
	{ "VoltageNotRegulated",		0x008500db },
	{ "MasterMode",				0x008500dc },
	/* 0x008500dd-0x008500ef	=>	Reserved */
	{ "ChargerSelectorSupport",		0x008500f0 },
	{ "ChargerSpec",			0x008500f1 },
	{ "Level2",				0x008500f2 },
	{ "Level3",				0x008500f3 },
	/* 0x008500f4-0x008500ff	=>	Reserved */

	/* end of structure. */
	{ NULL, 0 }
};

usage_lkp_t nut_mge_usage_lkp[] = {
	{ "Undefined",				0xffff0000 },
	{ "STS",					0xffff0001 },
	{ "Environment",			0xffff0002 },
	{ "Statistic",				0xffff0003 },
	{ "StatisticSystem",		0xffff0004 },
	{ "USB",					0xffff0005 },
	/* 0xffff0005-0xffff000f	=>	Reserved */
	{ "Phase",				0xffff0010 },
	{ "PhaseID",				0xffff0011 },
	{ "Chopper",				0xffff0012 },
	{ "ChopperID",				0xffff0013 },
	{ "Inverter",				0xffff0014 },
	{ "InverterID",				0xffff0015 },
	{ "Rectifier",				0xffff0016 },
	{ "RectifierID",			0xffff0017 },
	{ "LCMSystem",				0xffff0018 },
	{ "LCMSystemID",			0xffff0019 },
	{ "LCMAlarm",				0xffff001a },
	{ "LCMAlarmID",				0xffff001b },
	{ "HistorySystem",			0xffff001c },
	{ "HistorySystemID",			0xffff001d },
	{ "Event",				0xffff001e },
	{ "EventID",				0xffff001f },
	{ "CircuitBreaker",			0xffff0020 },
	{ "TransferForbidden",			0xffff0021 },
	{ "OverallAlarm",			0xffff0022 }, /* renamed to Alarm in Eaton SW! */
	{ "Dephasing",				0xffff0023 },
	{ "BypassBreaker",			0xffff0024 },
	{ "PowerModule",			0xffff0025 },
	{ "PowerRate",				0xffff0026 },
	{ "PowerSource",			0xffff0027 },
	{ "CurrentPowerSource",			0xffff0028 },
	{ "RedundancyLevel",			0xffff0029 },
	{ "RedundancyLost",			0xffff002a },
	{ "NotificationStatus",			0xffff002b },
	{ "ProtectionLost",			0xffff002c },
	{ "ConfigurationFailure",			0xffff002d },
	{ "CompatibilityFailure",			0xffff002e },
	/* 0xffff002e-0xffff003f	=>	Reserved */
	{ "SwitchType",				0xffff0040 }, /* renamed to Type in Eaton SW! */
	{ "ConverterType",			0xffff0041 },
	{ "FrequencyConverterMode",		0xffff0042 },
	{ "AutomaticRestart",			0xffff0043 },
	{ "ForcedReboot",			0xffff0044 },
	{ "TestPeriod",				0xffff0045 },
	{ "EnergySaving",			0xffff0046 },
	{ "StartOnBattery",			0xffff0047 },
	{ "Schedule",				0xffff0048 },
	{ "DeepDischargeProtection",		0xffff0049 },
	{ "ShortCircuit",			0xffff004a },
	{ "ExtendedVoltageMode",		0xffff004b },
	{ "SensitivityMode",			0xffff004c },
	{ "RemainingCapacityLimitSetting",	0xffff004d },
	{ "ExtendedFrequencyMode",		0xffff004e },
	{ "FrequencyConverterModeSetting",	0xffff004f },
	{ "LowVoltageBoostTransfer",		0xffff0050 },
	{ "HighVoltageBoostTransfer",		0xffff0051 },
	{ "LowVoltageBuckTransfer",		0xffff0052 },
	{ "HighVoltageBuckTransfer",		0xffff0053 },
	{ "OverloadTransferEnable",		0xffff0054 },
	{ "OutOfToleranceTransferEnable",	0xffff0055 },
	{ "ForcedTransferEnable",		0xffff0056 },
	{ "LowVoltageBypassTransfer",		0xffff0057 },
	{ "HighVoltageBypassTransfer",		0xffff0058 },
	{ "FrequencyRangeBypassTransfer",	0xffff0059 },
	{ "LowVoltageEcoTransfer",		0xffff005a },
	{ "HighVoltageEcoTransfer",		0xffff005b },
	{ "FrequencyRangeEcoTransfer",		0xffff005c },
	{ "ShutdownTimer",			0xffff005d },
	{ "StartupTimer",			0xffff005e },
	{ "RestartLevel",			0xffff005f },
	{ "PhaseOutOfRange", 			0xffff0060 },
	{ "CurrentLimitation", 			0xffff0061 },
	{ "ThermalOverload", 			0xffff0062 },
	{ "SynchroSource", 			0xffff0063 },
	{ "FuseFault", 				0xffff0064 },
	{ "ExternalProtectedTransfert", 	0xffff0065 },
	{ "ExternalForcedTransfert", 		0xffff0066 },
	{ "Compensation", 			0xffff0067 },
	{ "EmergencyStop", 			0xffff0068 },
	{ "PowerFactor", 			0xffff0069 },
	{ "PeakFactor", 			0xffff006a },
	{ "ChargerType", 			0xffff006b },
	{ "HighPositiveDCBusVoltage", 		0xffff006c },
	{ "LowPositiveDCBusVoltage", 		0xffff006d },
	{ "HighNegativeDCBusVoltage", 		0xffff006e },
	{ "LowNegativeDCBusVoltage", 		0xffff006f },
	{ "FrequencyRangeTransfer", 		0xffff0070 },
	{ "WiringFaultDetection", 		0xffff0071 },
	{ "ControlStandby", 			0xffff0072 },
	{ "ShortCircuitTolerance", 		0xffff0073 },
	{ "VoltageTooHigh", 			0xffff0074 },
	{ "VoltageTooLow", 			0xffff0075 },
	{ "DCBusUnbalanced", 			0xffff0076 },
	{ "FanFailure", 			0xffff0077 },
	{ "WiringFault", 			0xffff0078 },
	{ "Floating", 			0xffff0079 },
	{ "OverCurrent", 			0xffff007a },
	{ "RemainingActivePower", 			0xffff007b },
	{ "Energy", 			0xffff007c },
	{ "Threshold", 			0xffff007d },
	{ "OverThreshold", 			0xffff007e },
	/* 0xffff007f	=>	Reserved */
	{ "Sensor",				0xffff0080 },
	{ "LowHumidity",			0xffff0081 },
	{ "HighHumidity",			0xffff0082 },
	{ "LowTemperature",			0xffff0083 },
	{ "HighTemperature",			0xffff0084 },
	{ "ECOControl",			0xffff0085 },
	{ "Efficiency",			0xffff0086 },
	{ "ABMEnable",			0xffff0087 },
	{ "NegativeCurrent",	0xffff0088 },
	{ "AutomaticStart",		0xffff0089 },
	/* 0xffff008a-0xffff008f	=>	Reserved */
	{ "Count",				0xffff0090 },
	{ "Timer",				0xffff0091 },
	{ "Interval",				0xffff0092 },
	{ "TimerExpired",			0xffff0093 },
	{ "Mode",				0xffff0094 },
	{ "Country",				0xffff0095 },
	{ "State",				0xffff0096 },
	{ "Time",				0xffff0097 },
	{ "Code",				0xffff0098 },
	{ "DataValid",				0xffff0099 },
	{ "ToggleTimer",				0xffff009a },
	{ "BypassTransferDelay",		0xffff009b },
	{ "HysteresisVoltageTransfer",		0xffff009c },
	{ "SlewRate",					0xffff009d },
	/* 0xffff009e-0xffff009f	=>	Reserved */
	{ "PDU",					0xffff00a0 },
	{ "Breaker",				0xffff00a1 },
	{ "BreakerID",				0xffff00a2 },
	{ "OverVoltage",			0xffff00a3 },
	{ "Tripped",				0xffff00a4 },
	{ "OverEnergy",				0xffff00a5 },
	{ "OverHumidity",			0xffff00a6 },
	{ "ConfigurationReset",		0xffff00a7 }, /* renamed from LCDControl in Eaton SW! */
	{ "Level",			0xffff00a8 },
	{ "PDUType",			0xffff00a9 },
	{ "ReactivePower",			0xffff00aa },
	{ "Pole",			0xffff00ab },
	{ "PoleID",			0xffff00ac },
	{ "Reset",			0xffff00ad },
	{ "WatchdogReset",			0xffff00ae },
	/* 0xffff00af-0xffff00df	=>	Reserved */
	{ "iDesignator",			0xffff00ba },
	{ "COPIBridge",				0xffff00e0 },
	{ "Gateway",				0xffff00e1 },
	{ "System",					0xffff00e5 },
	{ "Status",					0xffff00e9 },
	/* 0xffff00ee-0xffff00ef	=>	Reserved */
	{ "iModel",				0xffff00f0 },
	{ "iVersion",				0xffff00f1 },
	{ "iTechnicalLevel",		0xffff00f2 },
	{ "iPartNumber",			0xffff00f3 },
	{ "iReferenceNumber",		0xffff00f4 },
	{ "iGang",					0xffff00f5 },
	/* 0xffff00f6-0xffff00ff	=>	Reserved */

	/* end of table */
	{ NULL, 0 }
};


static usage_tables_t mge_utab[] = {
	nut_mge_usage_lkp,
	nut_hid_usage_lkp,
	NULL,
};

HIDData_t *nut_hid_find_object(HIDDesc_t *desc, const char *hidpath)
{
	HIDPath_t path;
	HIDData_t *item;

	if (desc == NULL || hidpath == NULL) {
		return NULL;
	}
	if (string_to_path(hidpath, &path, mge_utab) <= 0) {
		return NULL;
	}
	/* Match upstream: FEATURE items are the canonical source. */
	item = FindObject_with_Path(desc, &path, ITEM_FEATURE);
	if (item == NULL) {
		item = FindObject_with_Path(desc, &path, ITEM_INPUT);
	}
	return item;
}

double nut_hid_scale_value(const HIDData_t *data, long logical)
{
	double value;
	int8_t unit_expo;
	int i;

	if (data == NULL) {
		return 0.0;
	}

	value = logical_to_physical((HIDData_t *)data, logical);

	unit_expo = data->UnitExp;
	for (i = 0; i < NB_HID_UNITS; i++) {
		if (HIDUnits[i].Type == data->Unit) {
			unit_expo -= HIDUnits[i].Expo;
			break;
		}
	}

	return value * exponent(10, unit_expo);
}
