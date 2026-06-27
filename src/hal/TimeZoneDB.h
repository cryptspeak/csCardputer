#pragma once

// =============================================================================
// TimeZoneDB — coarse lat/lon -> POSIX TZ rule lookup
//
// This is NOT a political-boundary-accurate timezone database (no polygon
// data, no IANA tzdata) — it's a small table of rectangular lat/lon boxes
// over major population centers, each carrying a real POSIX TZ rule string
// (the same kind already used by the old timezone picker), so a region that
// *is* covered gets exact DST transition dates, not a guess. Anywhere not
// covered by a box (oceans, poles, sparsely-populated regions) has no entry
// here — callers fall back to a cruder longitude/hemisphere estimate (see
// GPSManager::updateLocalTimeZone).
//
// Boxes are plain rectangles, so they don't follow real borders — a few
// degrees on either side of a boundary can land in the "wrong" neighboring
// box. Ordering matters: more specific/smaller carve-outs are listed before
// the broader boxes they sit inside of (e.g. Phoenix before the rest of the
// US Mountain zone, since Arizona doesn't observe DST), and lookupPosixTZ()
// returns the first match.
// =============================================================================

struct TZRegion {
    float latMin, latMax, lonMin, lonMax;
    const char* posixTZ;
};

static const TZRegion TZ_REGIONS[] = {
    // --- North America ---
    {31.0f, 37.0f, -114.8f, -109.0f, "MST7"},                              // Arizona (no DST)
    {18.0f, 23.0f, -161.0f, -154.0f, "HST10"},                             // Hawaii
    {51.0f, 72.0f, -170.0f, -129.0f, "AKST9AKDT,M3.2.0,M11.1.0"},          // Alaska
    {32.0f, 60.0f, -125.0f, -114.0f, "PST8PDT,M3.2.0,M11.1.0"},            // US/Canada Pacific
    {31.0f, 60.0f, -114.0f, -101.0f, "MST7MDT,M3.2.0,M11.1.0"},            // US/Canada Mountain
    {25.0f, 60.0f, -101.0f,  -87.0f, "CST6CDT,M3.2.0,M11.1.0"},            // US/Canada Central
    {24.0f, 52.0f,  -87.0f,  -66.0f, "EST5EDT,M3.2.0,M11.1.0"},            // US/Canada Eastern
    {43.0f, 52.0f,  -66.0f,  -59.0f, "AST4ADT,M3.2.0,M11.1.0"},            // Atlantic Canada
    {17.0f, 19.0f,  -68.0f,  -65.0f, "AST4"},                              // Puerto Rico / E. Caribbean (no DST)
    {14.0f, 24.0f, -106.0f,  -86.0f, "CST6"},                              // Mexico (most of, no DST since 2022)

    // --- South America ---
    {-34.0f,  5.0f, -58.0f, -34.0f, "<-03>3"},                             // Brazil (most of, fixed -3)
    {-55.0f, -21.0f, -73.0f, -53.0f, "<-03>3"},                            // Argentina (fixed -3)

    // --- Europe ---
    // Turkey and Russia/Moscow are listed before Eastern Europe even though
    // their boxes overlap it — both are fixed offsets with no DST, and sit
    // inside/against a box that *does* have DST, so (like Phoenix above)
    // the more specific no-DST carve-out has to win the overlap.
    {36.0f, 42.0f,  26.0f,  45.0f, "<+03>-3"},                            // Turkey (fixed +3, no DST since 2016)
    {41.0f, 70.0f,  27.0f,  50.0f, "MSK-3"},                              // Western Russia / Moscow (fixed +3, no DST)
    {49.0f, 61.0f, -11.0f,   2.0f, "GMT0BST,M3.5.0/1,M10.5.0"},            // UK/Ireland
    {36.0f, 60.0f,  -9.0f,  19.0f, "CET-1CEST,M3.5.0,M10.5.0/3"},          // Western/Central Europe
    // Capped at lat 53 / lon 30 to stay clear of Belarus and NW Russia
    // (both fixed +3, no DST) — this box is meant for Romania/Bulgaria/
    // Moldova/most of Ukraine, not the whole way up to the Baltics.
    {34.0f, 53.0f,  19.0f,  30.0f, "EET-2EEST,M3.5.0/3,M10.5.0/4"},        // Eastern Europe (EU DST rule)

    // --- Africa ---
    {22.0f, 32.0f,  24.0f,  37.0f, "EET-2"},                               // Egypt (fixed +2, no DST)
    {-35.0f, -22.0f, 16.0f,  33.0f, "SAST-2"},                             // South Africa (fixed +2, no DST)

    // --- Middle East / Central Asia ---
    {22.0f, 27.0f,  51.0f,  56.0f, "<+04>-4"},                            // UAE (fixed +4, no DST)
    {16.0f, 32.0f,  34.0f,  56.0f, "<+03>-3"},                            // Saudi Arabia (fixed +3, no DST)
    {24.0f, 37.0f,  61.0f,  75.0f, "PKT-5"},                              // Pakistan (fixed +5, no DST)

    // --- South / Southeast Asia ---
    { 6.0f, 36.0f,  68.0f,  97.0f, "IST-5:30"},                           // India (fixed +5:30, no DST)
    {20.0f, 27.0f,  88.0f,  93.0f, "<+06>-6"},                           // Bangladesh (fixed +6)
    { 1.0f,  7.0f, 100.0f, 104.0f, "<+08>-8"},                           // Singapore/Malaysia (fixed +8)
    { 5.0f, 21.0f,  97.0f, 106.0f, "<+07>-7"},                           // Thailand (fixed +7)
    {-11.0f, 6.0f,  95.0f, 114.0f, "<+07>-7"},                           // Indonesia (western, fixed +7)
    {18.0f, 53.0f,  73.0f, 135.0f, "<+08>-8"},                           // China (single official zone, fixed +8)

    // --- East Asia ---
    {24.0f, 46.0f, 129.0f, 146.0f, "JST-9"},                             // Japan (fixed +9, no DST)
    {33.0f, 39.0f, 124.0f, 132.0f, "KST-9"},                             // South Korea (fixed +9, no DST)

    // --- Australia / Oceania ---
    {-36.0f, -13.0f, 112.0f, 129.0f, "AWST-8"},                                       // Western Australia (fixed +8, no DST)
    {-38.0f, -25.0f, 129.0f, 141.0f, "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},               // South/Central Australia
    {-44.0f,  -9.0f, 141.0f, 154.0f, "AEST-10AEDT,M10.1.0,M4.1.0/3"},                 // Eastern Australia
    {-47.0f, -34.0f, 166.0f, 179.0f, "NZST-12NZDT,M9.5.0,M4.1.0/3"},                  // New Zealand
};

static constexpr int TZ_REGION_COUNT = sizeof(TZ_REGIONS) / sizeof(TZ_REGIONS[0]);

// Returns the POSIX TZ rule string for the first matching box, or nullptr
// if (lat, lon) falls outside every covered region.
inline const char* lookupPosixTZ(double lat, double lon) {
    for (int i = 0; i < TZ_REGION_COUNT; i++) {
        const TZRegion& r = TZ_REGIONS[i];
        if (lat >= r.latMin && lat <= r.latMax && lon >= r.lonMin && lon <= r.lonMax) {
            return r.posixTZ;
        }
    }
    return nullptr;
}
