// ecotracker.h — EcoTracker IR emulation: makes this gateway look like an everHome EcoTracker on the
// LAN so an EcoFlow STREAM (Ultra / Ultra X / AC Pro) adopts it as its grid meter and runs its own
// zero-export control loop against a reader's readings — no cloud, no extra hardware.
//
// Why the EcoTracker and not a Shelly 3EM: EcoFlow reads a Shelly through Shelly's *cloud* (proprietary,
// encrypted uplink), so an emulated Shelly is discovered and then stays "connection pending" forever. The
// EcoTracker path is local-only by design — recent EcoFlow firmware pairs it exclusively over mDNS and no
// longer even accepts a manually typed meter IP. See the "EcoFlow STREAM" section in README.md.
//
// Everything is served from the existing dashboard WebServer on port 80; the only new surface is one
// unauthenticated GET /v1/json (EcoFlow's inverter cannot log in). Off by default.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

void   eco_init(WebServer *srv);   // load the saved config; call once before the routes are registered
void   eco_handle_v1json();        // GET /v1/json  — the EcoTracker local API (register UNGUARDED)
void   eco_handle_cfg();           // POST /api/ecotracker — enable/select reader (register behind guard())
void   eco_service(bool wifiUp);   // (re)publish the mDNS record when WiFi/IP changes; call from the web loop
String eco_status_json();          // the "ecotracker":{…} object for /api/status
bool   eco_enabled();
