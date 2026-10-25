/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
// A real original MyBrewbot config: the files from the LittleFS partition of a
// controller still running the original firmware (flash dump 2026-03-29). Only
// the secrets are replaced - the Blynk token ("authcode"), the Brewfather ID
// ("brewserviceid") and everything in jsonConfig.txt. Every other byte is as
// the original firmware wrote it, including its formatting.

static const char ORIG_GLOBAL[] =
  R"({"authcode":"DummyBlynkToken0000000000000000","unit":1,"notifyon":true,"brewserviceid":"DummyBrewId123","brewservice":3,"migrated":false,"blebaud":9600,"lastuptime":176,"swno":20,"sendtocloud":0,"globalsave":0,"mybrewbuddy":"","bsitterauth":"","babysitter":0,"plugcategory":true,"fno":1,"mbbhardreset":0,"tuning_chart_no":0,"resolution":11})";

static const char ORIG_FERMENTER[] =
  R"({"Fermenter":[0,0,0,0],"FermenterName":["Fermenter","Fermenter","Fermenter","Fermenter"],"BeerName":["Bitter","Beer","Beer","Beer"],"YeastName":["S04","Yeast","Yeast","Yeast"],"BJCP":["BJCP","BJCP","BJCP","BJCP"],"CeilingTemp":[2,20,20,20],"FloorTemp":[1,20,20,20],"OG":[1047,1050,1050,1050],"TG":[1010,1010,1010,1010],"Hysteresis":[0.1,0,0,0],"CompressorDelay":[10,0,0,0],"TempControl":[1,1,1,1],"SGControl":[1,1,1,1],"Power":[0,1,1,1],"AlarmTolerance":[0,0,0,0],"AmbientSG":[70903,0,0,0],"Alarm":[0,0,0,0],"ProfileNo":[0,0,0,0],"CurrentHour":[0,0,0,0],"LiveTest":[1,1,1,1],"Status":[0,0,0,0],"ProfileRunning":[0,0,0,0],"BrewServiceSend":[1,1,1,1],"PSI_Collect":[0,0,0,0],"Function":[1,1,1,1],"Series1":[1,1,1,1],"Series2":[3,3,3,3],"Series3":[8,8,8,8],"Series4":[2,2,2,2],"SGCalibration":[0.001489,0.001489,0.001489,0.001489],"MyBrewBuddyPSI_Colle":[0,0,0,0],"StartMillis":[0,0,0,0]})";

static const char ORIG_PROBE[] =
  R"({"Probe_Name":["Beer Temp","Fridge Temp","Probe","Probe","Probe","Probe","Probe","Probe"],"Address":["28805555a0080","28ce2555a008b","","","","","",""],"Function":[2,3,99,99,99,99,99,99],"Fermenter":[0,0,99,99,99,99,99,99],"Temperature":[13,13,0,0,0,0,0,0],"MBB":[0,0,0,0,0,0,0,0]})";

static const char ORIG_SMARTPLUGS[] =
  R"({"Type":[1,1,1,1,1,1,1,1,1,1],"Codeset":[3,3,1,1,1,1,1,1,1,1],"Protocol":[1,1,1,1,1,1,1,1,1,1],"Bits":[24,24,24,24,24,24,24,24,24,24],"DelayLength":[427,427,160,160,160,160,160,160,160,160],"Function":[99,99,99,99,99,99,99,99,99,99],"Fermenter":[99,99,99,99,99,99,99,99,99,99],"OnCode":[1,2,0,0,0,0,0,0,0,0],"OffCode":[1,2,0,0,0,0,0,0,0,0],"Manufacturer":["Dial","Dial","Manufacturer","Manufacturer","Manufacturer","Manufacturer","Manufacturer","Manufacturer","Manufacturer","Manufacturer"],"Model":["Model","Model","Model","Model","Model","Model","Model","Model","Model","Model"],"PlugNo":[0,0,0,0,0,0,0,0,0,0]})";

static const char ORIG_PROFILE[] =
  R"({"ProfileName":["Empty Profile","Empty Profile","Empty Profile","Empty Profile"]})";

static const char ORIG_PROFILESTEPS[] =
  R"({"StepNo":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"StepType":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"StartTemp":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"EndTemp":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"SGTrigger":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"Days":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]})";

static const char ORIG_TILT[] =
  R"({"Address":[99,99,99,99],"Function":[99,99,99,99],"Fermenter":[99,99,99,99],"Temp_Adjust":[0,0,0,0],"SG_Adjust":[0,0,0,0],"MBB":[0,0,0,0]})";

static const char ORIG_ISPINDEL[] =
  R"({"iSpindelName":["None","None","None","None"],"ID":[0,0,0,0],"iSpindelCollectData":[0,0,0,0],"iSpindelFermenter":[0,99,99,99],"Unit":[1,0,0,0]})";

static const char ORIG_REBOOT[] =
  R"({"UTC":[1742652137,1734648450,1733891195,1733853348,0,1733844213,1733838292,1733838292,1711282449,1709371305],"RebootReason":[1,5,3,1,1,5,1,1,1,1]})";

static const char ORIG_PRESSURE[] =
  R"({"PSI_0":[160,160],"PSI_5":[190,190],"PSI_10":[220,220],"MBB":[0,0],"PSI_15":[250,250],"PSI_20":[280,280],"PSIalarmSwitch":[0,0],"AlarmPSI":[0,0]})";

static const char ORIG_PLAATO[] =
  R"({"AuthCode":["Plaato Authcode","Plaato Authcode","Plaato Authcode","Plaato Authcode"],"GetData":[0,0,0,0]})";

static const char ORIG_REPEATER[] =
  R"({"MyBrewBuddyOn":[0,0,0,0],"MyBrewBuddy":["","","",""],"MyBrewBuddyMac":["","","",""]})";

static const char ORIG_CHARTNAMES[] =
  R"({"title":["Fermenter 1","Fermenter","Fermenter","Fermenter"],"subtitle":["Bitter","Beer","Beer","Beer"]})";

static const char ORIG_CHARTSERIES[] =
  R"({"Series":[1,3,7,8,10,10,10,10,10,10,10,10,10,10,10,10],"Source":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]})";

// Same keys as the real file (Blynk Edgent's config); the values are made up
static const char ORIG_CONFIG[] =
  R"({"magic":1,"version":1,"flagConfig":1,"flagApFail":0,"flagSelfTest":0,"wifiSSID":"DummySSID","wifiPass":"DummyWiFiPassword","cloudToken":"DummyBlynkToken0000000000000000","cloudHost":"blynk.cloud","cloudPort":80,"checksum":0})";
