// ecotracker.cpp — everHome EcoTracker IR emulation (see include/ecotracker.h for the why).
//
// Two surfaces, both tiny:
//   mDNS   _everhome._tcp.local., instance/host "ecotracker-<mac>", TXT ip/serial/productid=1137
//   HTTP   GET /v1/json on the dashboard's own port-80 server
//
// The protocol contract comes from everHome's published Local API plus two independent emulators
// (sdeigm/uni-meter, wwerther/ha-ecotracker-emulator); it is not guessed. Field units and the sign
// convention line up with what the reader already gives us:
//   power            W, POSITIVE = importing from the grid, negative = exporting  (same as Reader::power)
//   energyCounter*   WATT-hours (not kWh), monotonically increasing               (same as Reader::import_)
// so the emulation is a re-labelling of existing values, not a conversion with rounding to get wrong.
#include "ecotracker.h"
#include "reader.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

// Real EcoTrackers all sit under everHome's OUI. EcoFlow may or may not care, but the emulators that are
// known to work in the field keep it, and there is no cost to matching — only the device half is ours.
static const char ECOTRACKER_OUI[] = "B43A45";
static const char ECOTRACKER_PRODUCTID[] = "1137";   // observed on a real device (ha-ecotracker-emulator)

// ---------------------------------------------------------------------------- config (NVS ns "obieco")
static WebServer *g_srv = nullptr;
static Preferences ecoPrefs;

static bool     g_ecoOn = false;          // master switch — OFF by default; nothing is advertised until asked
static uint8_t  g_ecoHandle[3] = {0};     // reader to publish; 00 00 00 = "auto: first assigned reader with data"
static uint8_t  g_ecoSrc = 0;             // 0 = auto (reported W, fall back to calculated), 1 = reported, 2 = calculated
static uint16_t g_ecoStale = 300;         // serve 503 once the reading is older than this many seconds (0 = never)
// Which powerPhaseN field carries the reading (0/1/2 = L1/L2/L3). A single-phase reader knows nothing about
// phases, so this is purely how the STREAM sees it; it regulates on the total `power` either way.
static uint8_t  g_ecoPhase = 0;
static char     g_ecoSerial[13] = "";     // 12 hex chars, stable across reboots (the app pairs against it)
// Optional client allowlist for the unauthenticated /v1/json: comma-separated IPv4 addresses and/or CIDR
// prefixes ("192.168.1.50, 10.0.0.0/8"). Empty = anyone on the LAN (the default -- the endpoint only leaks
// one meter's W/Wh, and the STREAM's address is usually DHCP-assigned, so a mandatory list would just break
// installs). Non-matching clients get 403; the STREAM itself is never denied a 404/503 it would otherwise get.
static char     g_ecoAllow[128] = "";
static uint32_t g_denied = 0;             // /v1/json requests refused by the allowlist (surfaced in the UI)

// Counter continuity across a reader switch. A grid meter's totals may never go backwards -- a consumer
// that sees them drop can reject the meter outright or corrupt its own energy statistics. Switching the
// published reader (or a reader being re-based onto a replaced meter) is exactly such a drop, so carry an
// offset that keeps the published series monotonic instead of passing the raw counters straight through.
static int64_t  g_offIn = 0, g_offOut = 0;
static uint32_t g_lastIn = 0, g_lastOut = 0;
static bool     g_haveLast = false;

// mDNS bookkeeping — the record has to be republished when the IP changes (DHCP lease, reconnect, the
// static-IP setting being turned on), otherwise EcoFlow keeps polling an address we no longer hold.
static bool      g_mdnsUp = false;
static IPAddress g_mdnsIp;
static uint32_t  g_lastPollMs = 0;   // last GET /v1/json — the UI's proof that the STREAM is actually talking
static uint32_t  g_pollCount = 0;

static String jstr(const char *s) { String o = "\""; for (; *s; s++) { if (*s == '"' || *s == '\\') o += '\\'; o += *s; } return o + "\""; }

// "B43A45" + this device's own last 3 MAC bytes: unique per gateway, stable, and still inside the vendor OUI.
static const char *ecoMac() {
  static char mac[13];
  if (!mac[0]) { uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
                 snprintf(mac, sizeof mac, "%s%02X%02X%02X", ECOTRACKER_OUI, m[3], m[4], m[5]); }
  return mac;
}
static const char *ecoHostname() {   // "ecotracker-b43a45xxxxxx" — mDNS hostname AND service instance name
  static char h[32];
  if (!h[0]) { snprintf(h, sizeof h, "ecotracker-%s", ecoMac()); for (char *p = h; *p; p++) *p = tolower(*p); }
  return h;
}

static String handleHex(const uint8_t h[3]) {
  char b[7]; snprintf(b, sizeof b, "%02x%02x%02x", h[0], h[1], h[2]); return String(b);
}

static void saveOffsets() {
  ecoPrefs.begin("obieco", false);
  ecoPrefs.putLong64("offin", g_offIn); ecoPrefs.putLong64("offout", g_offOut);
  ecoPrefs.putUInt("lastin", g_lastIn); ecoPrefs.putUInt("lastout", g_lastOut);
  ecoPrefs.end();
}

void eco_init(WebServer *srv) {
  g_srv = srv;
  ecoPrefs.begin("obieco", true);
  g_ecoOn    = ecoPrefs.getBool("on", false);
  g_ecoSrc   = ecoPrefs.getUChar("src", 0);
  g_ecoStale = ecoPrefs.getUShort("stale", 300);
  g_ecoPhase = ecoPrefs.getUChar("phase", 0); if (g_ecoPhase > 2) g_ecoPhase = 0;
  { String s = ecoPrefs.getString("serial", ""); s.toCharArray(g_ecoSerial, sizeof g_ecoSerial); }
  { String s = ecoPrefs.getString("allow", "");  s.toCharArray(g_ecoAllow,  sizeof g_ecoAllow);  }
  { size_t n = ecoPrefs.getBytes("rdr", g_ecoHandle, 3); if (n != 3) memset(g_ecoHandle, 0, 3); }
  g_offIn  = ecoPrefs.getLong64("offin", 0);  g_offOut  = ecoPrefs.getLong64("offout", 0);
  g_lastIn = ecoPrefs.getUInt("lastin", 0);   g_lastOut = ecoPrefs.getUInt("lastout", 0);
  g_haveLast = (g_lastIn || g_lastOut);
  ecoPrefs.end();
  if (strlen(g_ecoSerial) != 12) {   // 12 hex chars, independent of the MAC (real-device convention).
    // Generated once and persisted: the EcoFlow app pairs against this serial, so a serial that changed
    // on every boot would show up as a brand-new meter each time and need re-pairing.
    snprintf(g_ecoSerial, sizeof g_ecoSerial, "%06x%06x", (unsigned)(esp_random() & 0xFFFFFF), (unsigned)(esp_random() & 0xFFFFFF));
    ecoPrefs.begin("obieco", false); ecoPrefs.putString("serial", g_ecoSerial); ecoPrefs.end();
  }
}

bool eco_enabled() { return g_ecoOn; }

// ---------------------------------------------------------------------------- reader selection
// Explicitly configured handle, or -- when none is set -- the first assigned reader that actually has
// energy data. "Auto" is what makes a single-reader install work with nothing but the on/off switch.
static Reader *ecoReader() {
  bool any = !(g_ecoHandle[0] || g_ecoHandle[1] || g_ecoHandle[2]);
  for (int i = 0; i < MAX_READERS; i++) {
    Reader &r = readers[i];
    if (!r.used || !r.assigned) continue;
    if (any) { if (r.haveData) return &r; }
    else if (memcmp(r.handle, g_ecoHandle, 3) == 0) return &r;
  }
  return nullptr;
}

// Live power in W (positive = import), or 0x7FFFFFFF when this reader currently has no usable value.
// calcPower is the average implied by the Wh counters themselves, so it stays right even on meters whose
// own instantaneous power reading is garbage (see the SML sign-extension bugs in ../reader_firmware_mod).
static uint32_t ecoPower(const Reader &r) {
  if (g_ecoSrc == 1) return r.power;
  if (g_ecoSrc == 2) return r.calcPower;
  return obi_na(r.power) ? r.calcPower : r.power;
}

// ---------------------------------------------------------------------------- allowlist
// One entry: "a.b.c.d" or "a.b.c.d/n". Returns false for anything unparseable, so a typo denies rather than
// silently opens up -- and the settings handler rejects such lists up front so a typo can't be saved.
static bool parseAllowEntry(const char *e, size_t len, uint32_t &net, uint32_t &mask) {
  char b[24]; if (len == 0 || len >= sizeof b) return false;
  memcpy(b, e, len); b[len] = 0;
  int bits = 32; char *slash = strchr(b, '/');
  if (slash) { *slash = 0; char *end; long v = strtol(slash + 1, &end, 10); if (*end || v < 0 || v > 32) return false; bits = (int)v; }
  IPAddress ip; if (!ip.fromString(b)) return false;
  net  = (uint32_t)ip;                               // Arduino IPAddress -> uint32 is in NETWORK byte order ...
  mask = bits == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - bits));   // ... so build the mask the same way
  net &= mask;
  return true;
}
// Walks the list. validateOnly: just check the syntax (client ignored). Empty list = everyone allowed.
static bool allowlistCheck(const char *list, IPAddress client, bool validateOnly) {
  const char *p = list; bool anyEntry = false;
  while (*p) {
    while (*p == ' ' || *p == ',' || *p == ';') p++;
    if (!*p) break;
    const char *e = p; while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
    uint32_t net, mask;
    if (!parseAllowEntry(e, p - e, net, mask)) return false;
    anyEntry = true;
    if (!validateOnly && (((uint32_t)client & mask) == net)) return true;
  }
  return validateOnly ? true : !anyEntry;
}

// ---------------------------------------------------------------------------- GET /v1/json
void eco_handle_v1json() {
  if (!g_ecoOn) { g_srv->send(404, "application/json", "{\"error\":\"ecotracker emulation disabled\"}"); return; }
  if (g_ecoAllow[0] && !allowlistCheck(g_ecoAllow, g_srv->client().remoteIP(), false)) {
    g_denied++;
    Serial.printf("[eco] /v1/json refused for %s (not in allowlist)\n", g_srv->client().remoteIP().toString().c_str());
    g_srv->send(403, "application/json", "{\"error\":\"client not allowed\"}");
    return;
  }
  Reader *rp = ecoReader();
  if (!rp || !rp->haveData || obi_na(rp->import_)) {
    g_srv->send(503, "application/json", "{\"error\":\"no reader data\"}");
    return;
  }
  Reader &r = *rp;
  uint32_t age = (millis() - r.lastEnergyMs) / 1000;
  uint32_t pw  = ecoPower(r);
  // Deliberately an ERROR, not a stale value or a helpful-looking 0: the STREAM regulates against whatever
  // we return, so publishing a reading we no longer believe would have it push real power around based on
  // fiction. A reader reports on an interval (minutes), so being seconds-old is normal -- that is what
  // agePower below is for; only genuinely dead data is refused.
  if (obi_na(pw) || (g_ecoStale && age > g_ecoStale)) {
    g_srv->send(503, "application/json", String("{\"error\":\"stale\",\"age_s\":") + age + "}");
    return;
  }

  // Monotonic guard (see g_offIn). Only ever ratchets the published counters forward.
  int64_t in  = (int64_t)r.import_ + g_offIn;
  int64_t out = (int64_t)(obi_na(r.export_) ? 0 : r.export_) + g_offOut;
  if (g_haveLast && (in < (int64_t)g_lastIn || out < (int64_t)g_lastOut)) {
    if (in  < (int64_t)g_lastIn)  { g_offIn  += (int64_t)g_lastIn  - in;  in  = g_lastIn; }
    if (out < (int64_t)g_lastOut) { g_offOut += (int64_t)g_lastOut - out; out = g_lastOut; }
    Serial.printf("[eco] counters went backwards (reader switch?) -- offsets now in=%lld out=%lld\n",
                  (long long)g_offIn, (long long)g_offOut);
    g_lastIn = (uint32_t)in; g_lastOut = (uint32_t)out; g_haveLast = true;
    saveOffsets();
  } else if (!g_haveLast || (uint32_t)in != g_lastIn || (uint32_t)out != g_lastOut) {
    g_lastIn = (uint32_t)in; g_lastOut = (uint32_t)out; g_haveLast = true;
    // not persisted on every tick -- only the offsets matter across a reboot, and the counters they
    // apply to come from the reader, which keeps its own totals anyway.
  }

  long p = (long)(int32_t)pw;
  // powerAvg is specified as the mean over the last 60 s. calcPower is exactly that -- an average over a
  // >= 60 s window of real counter movement -- so use it when it is available rather than inventing a
  // second averaging path here; fall back to the instantaneous value when it is not.
  long pavg = obi_na(r.calcPower) ? p : (long)(int32_t)r.calcPower;

  long ph[3] = {0, 0, 0}; ph[g_ecoPhase] = p;

  char buf[320];
  snprintf(buf, sizeof buf,
           "{\"power\":%ld,\"powerAvg\":%ld,\"agePower\":%lu,"
           "\"powerPhase1\":%ld,\"powerPhase2\":%ld,\"powerPhase3\":%ld,"
           "\"energyCounterIn\":%lld,\"energyCounterInT1\":%lld,\"energyCounterInT2\":0,"
           "\"energyCounterOut\":%lld}",
           p, pavg, (unsigned long)age, ph[0], ph[1], ph[2], (long long)in, (long long)in, (long long)out);
  g_lastPollMs = millis(); g_pollCount++;
  g_srv->send(200, "application/json", buf);
}

// ---------------------------------------------------------------------------- mDNS
// EcoFlow's app pairs EXCLUSIVELY through mDNS discovery (it no longer lets you type a meter IP), so this
// record is not a convenience -- without it the meter cannot be added at all.
static void ecoMdnsStop() {
  if (!g_mdnsUp) return;
  MDNS.end();
  g_mdnsUp = false;
  Serial.println("[eco] mDNS record withdrawn");
}
static void ecoMdnsStart(IPAddress ip) {
  ecoMdnsStop();
  if (!MDNS.begin(ecoHostname())) { Serial.println("[eco] mDNS start FAILED"); return; }
  MDNS.setInstanceName(ecoHostname());
  MDNS.addService("everhome", "tcp", 80);
  MDNS.addServiceTxt("everhome", "tcp", "ip", ip.toString().c_str());
  MDNS.addServiceTxt("everhome", "tcp", "serial", (const char *)g_ecoSerial);
  MDNS.addServiceTxt("everhome", "tcp", "productid", ECOTRACKER_PRODUCTID);
  g_mdnsUp = true; g_mdnsIp = ip;
  Serial.printf("[eco] advertising %s._everhome._tcp on %s:80 (serial %s, mac %s)\n",
                ecoHostname(), ip.toString().c_str(), g_ecoSerial, ecoMac());
}

void eco_service(bool wifiUp) {
  if (!g_ecoOn || !wifiUp) { ecoMdnsStop(); return; }
  IPAddress ip = WiFi.localIP();
  if (ip == IPAddress((uint32_t)0)) return;
  if (!g_mdnsUp || ip != g_mdnsIp) ecoMdnsStart(ip);
}

// ---------------------------------------------------------------------------- POST /api/ecotracker
void eco_handle_cfg() {
  bool wasOn = g_ecoOn;
  if (g_srv->hasArg("on")) g_ecoOn = g_srv->arg("on") == "1" || g_srv->arg("on") == "true";
  if (g_srv->hasArg("reader")) {
    String h = g_srv->arg("reader"); h.trim();
    if (!h.length() || h == "auto") memset(g_ecoHandle, 0, 3);
    else if (h.length() == 6) {
      for (int i = 0; i < 3; i++) g_ecoHandle[i] = (uint8_t)strtoul(h.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    } else { g_srv->send(400, "application/json", "{\"ok\":false,\"err\":\"reader\"}"); return; }
    // A different reader means a different meter, hence different totals: drop the continuity offsets
    // rather than silently carrying the old meter's correction over onto the new one. The monotonic guard
    // in /v1/json re-establishes them from the first published sample.
    g_offIn = g_offOut = 0; g_lastIn = g_lastOut = 0; g_haveLast = false;
  }
  if (g_srv->hasArg("src")) { int s = g_srv->arg("src").toInt(); if (s >= 0 && s <= 2) g_ecoSrc = (uint8_t)s; }
  if (g_srv->hasArg("stale")) { long s = g_srv->arg("stale").toInt(); if (s >= 0 && s <= 65535) g_ecoStale = (uint16_t)s; }
  if (g_srv->hasArg("phase")) { int s = g_srv->arg("phase").toInt(); if (s >= 0 && s <= 2) g_ecoPhase = (uint8_t)s; }
  if (g_srv->hasArg("allow")) {
    String a = g_srv->arg("allow"); a.trim();
    if (a.length() >= sizeof g_ecoAllow || !allowlistCheck(a.c_str(), IPAddress(), true)) {
      g_srv->send(400, "application/json", "{\"ok\":false,\"err\":\"allow\"}"); return;
    }
    a.toCharArray(g_ecoAllow, sizeof g_ecoAllow);
  }
  ecoPrefs.begin("obieco", false);
  ecoPrefs.putBool("on", g_ecoOn);
  ecoPrefs.putBytes("rdr", g_ecoHandle, 3);
  ecoPrefs.putUChar("src", g_ecoSrc);
  ecoPrefs.putUShort("stale", g_ecoStale);
  ecoPrefs.putUChar("phase", g_ecoPhase);
  ecoPrefs.putString("allow", g_ecoAllow);
  ecoPrefs.putLong64("offin", g_offIn); ecoPrefs.putLong64("offout", g_offOut);
  ecoPrefs.putUInt("lastin", g_lastIn); ecoPrefs.putUInt("lastout", g_lastOut);
  ecoPrefs.end();
  if (wasOn != g_ecoOn) {
    Serial.printf("[eco] EcoTracker emulation %s\n", g_ecoOn ? "enabled" : "disabled");
    eco_service(WiFi.status() == WL_CONNECTED);   // publish/withdraw the record immediately, not on the next loop
  }
  g_srv->send(200, "application/json", eco_status_json());
}

// ---------------------------------------------------------------------------- status
String eco_status_json() {
  Reader *r = ecoReader();
  String j = "{\"enabled\":" + String(g_ecoOn ? "true" : "false");
  j += ",\"reader\":" + jstr(handleHex(g_ecoHandle).c_str());   // "000000" = auto
  j += ",\"active_reader\":" + (r ? jstr(handleHex(r->handle).c_str()) : String("null"));
  j += ",\"src\":" + String(g_ecoSrc);
  j += ",\"stale_s\":" + String(g_ecoStale);
  j += ",\"phase\":" + String(g_ecoPhase);
  j += ",\"allow\":" + jstr(g_ecoAllow);
  j += ",\"denied\":" + String(g_denied);
  j += ",\"host\":" + jstr(ecoHostname());
  j += ",\"mac\":" + jstr(ecoMac());
  j += ",\"serial\":" + jstr(g_ecoSerial);
  j += ",\"advertised\":" + String(g_mdnsUp ? "true" : "false");
  j += ",\"port\":80";
  j += ",\"polls\":" + String(g_pollCount);
  j += ",\"last_poll_s\":" + String(g_lastPollMs ? (long)((millis() - g_lastPollMs) / 1000) : -1);
  { uint32_t pw = r ? ecoPower(*r) : 0x7FFFFFFF;
    j += ",\"power\":" + (r && !obi_na(pw) ? String((long)(int32_t)pw) : String("null")); }
  j += "}";
  return j;
}
