#pragma once

#include <string>

#include "cJSON.h"

// Dataset recording policy (PROTOCOL.md §2.1). Sent by the app with
// set_dataset; the RV validates it and stamps every recording with it.
struct Policy {
    double conf_threshold      = 0.80;   // minimum beat confidence for a usable beat
    bool   rhythm_check        = true;   // beat length within +-30 % of neighbour median
    double autostart_s         = 2.0;    // contiguous usable coverage before auto-start
    double target_usable_s     = 30.0;   // 0 = off
    int    target_usable_beats = 0;      // 0 = off
    int    min_run_beats       = 0;      // 0 = off
    double min_run_s           = 0.0;    // 0 = off
    double max_duration_s      = 180.0;
    double telemetry_hz        = 2.0;

    // Parse + validate. Missing keys keep defaults. On error returns false and
    // sets `err` to the offending field.
    static bool fromJson(const cJSON* o, Policy& out, std::string& err);
    cJSON* toJson() const;
};

struct ActiveDataset {
    std::string id;        // phone-generated UUID
    std::string name;
    int         rev = 0;   // settings revision
    Policy      policy;
    bool valid() const { return !id.empty(); }
};

inline bool Policy::fromJson(const cJSON* o, Policy& p, std::string& err) {
    p = Policy();
    if (!cJSON_IsObject(o)) return true;
    auto num = [&](const char* k, double& v, double lo, double hi) {
        const cJSON* j = cJSON_GetObjectItemCaseSensitive(o, k);
        if (!j) return true;
        if (!cJSON_IsNumber(j) || j->valuedouble < lo || j->valuedouble > hi) { err = k; return false; }
        v = j->valuedouble;
        return true;
    };
    auto inum = [&](const char* k, int& v, int lo, int hi) {
        double d = v;
        if (!num(k, d, lo, hi)) return false;
        v = static_cast<int>(d);
        return true;
    };
    const cJSON* rc = cJSON_GetObjectItemCaseSensitive(o, "rhythm_check");
    if (rc && !cJSON_IsBool(rc)) { err = "rhythm_check"; return false; }
    if (rc) p.rhythm_check = cJSON_IsTrue(rc);
    if (!num("conf_threshold", p.conf_threshold, 0.50, 0.99) ||
        !num("autostart_s", p.autostart_s, 0.0, 10.0) ||
        !num("target_usable_s", p.target_usable_s, 0.0, 600.0) ||
        !inum("target_usable_beats", p.target_usable_beats, 0, 2000) ||
        !inum("min_run_beats", p.min_run_beats, 0, 200) ||
        !num("min_run_s", p.min_run_s, 0.0, 300.0) ||
        !num("max_duration_s", p.max_duration_s, 10.0, 1800.0) ||
        !num("telemetry_hz", p.telemetry_hz, 0.5, 5.0))
        return false;
    if (p.target_usable_s <= 0 && p.target_usable_beats <= 0) { err = "target_usable_s"; return false; }
    if (p.min_run_s > 0 && p.target_usable_s > 0 && p.min_run_s > p.target_usable_s) { err = "min_run_s"; return false; }
    if (p.min_run_beats > 0 && p.target_usable_beats > 0 && p.min_run_beats > p.target_usable_beats) { err = "min_run_beats"; return false; }
    if (p.max_duration_s <= p.target_usable_s) { err = "max_duration_s"; return false; }
    return true;
}

inline cJSON* Policy::toJson() const {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "conf_threshold", conf_threshold);
    cJSON_AddBoolToObject(o, "rhythm_check", rhythm_check);
    cJSON_AddNumberToObject(o, "autostart_s", autostart_s);
    cJSON_AddNumberToObject(o, "target_usable_s", target_usable_s);
    cJSON_AddNumberToObject(o, "target_usable_beats", target_usable_beats);
    cJSON_AddNumberToObject(o, "min_run_beats", min_run_beats);
    cJSON_AddNumberToObject(o, "min_run_s", min_run_s);
    cJSON_AddNumberToObject(o, "max_duration_s", max_duration_s);
    cJSON_AddNumberToObject(o, "telemetry_hz", telemetry_hz);
    return o;
}
