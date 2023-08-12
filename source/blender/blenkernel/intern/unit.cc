/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BLI_math_base.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utf8_symbols.h"
#include "BLI_sys_types.h"

#include "DNA_scene_types.h"

#include "BKE_unit.h" /* own include */

#ifdef WIN32
#  include "BLI_winstuff.h"
#endif

/* No BKE or DNA includes! */

/* Keep alignment. */
/* clang-format off */


#define TEMP_STR_SIZE 256


#define SEP_CHR     '#'
#define SEP_STR     "#"


#define EPS 0.001


#define UN_SC_KM    1000.0f
#define UN_SC_HM    100.0f
#define UN_SC_DAM   10.0f
#define UN_SC_M     1.0f
#define UN_SC_DM    0.1f
#define UN_SC_CM    0.01f
#define UN_SC_MM    0.001f
#define UN_SC_UM    0.000001f


#define UN_SC_MI    1609.344f
#define UN_SC_FUR   201.168f
#define UN_SC_CH    20.1168f
#define UN_SC_YD    0.9144f
#define UN_SC_FT    0.3048f
#define UN_SC_IN    0.0254f
#define UN_SC_MIL   0.0000254f


#define UN_SC_MTON  1000.0f /* Metric ton. */
#define UN_SC_QL    100.0f
#define UN_SC_KG    1.0f
#define UN_SC_HG    0.1f
#define UN_SC_DAG   0.01f
#define UN_SC_G     0.001f
#define UN_SC_MG    0.000001f


#define UN_SC_ITON  907.18474f /* Imperial ton. */
#define UN_SC_CWT   45.359237f
#define UN_SC_ST    6.35029318f
#define UN_SC_LB    0.45359237f
#define UN_SC_OZ    0.028349523125f


#define UN_SC_FAH   0.555555555555f

/* clang-format on */

/* Define a single unit. */
struct bUnitDef {
  const char *name;
  /** Abused a bit for the display name. */
  const char *name_plural;
  /** This is used for display. */
  const char *name_short;
  /**
   * Keyboard-friendly ASCII-only version of name_short, can be nullptr.
   * If name_short has non-ASCII chars, name_alt should be present.
   */
  const char *name_alt;

  /** Can be nullptr. */
  const char *name_display;
  /** When nullptr, a transformed version of the name will be taken in some cases. */
  const char *identifier;

  double scalar;
  /** Needed for converting temperatures. */
  double bias;
  int flag;
};

enum {
  B_UNIT_DEF_NONE = 0,
  /** Use for units that are not used enough to be translated into for common use. */
  B_UNIT_DEF_SUPPRESS = 1,
  /** Display a unit even if its value is 0.1, eg 0.1mm instead of 100um. */
  B_UNIT_DEF_TENTH = 2,
  /** Short unit name is case sensitive, for example to distinguish mW and MW. */
  B_UNIT_DEF_CASE_SENSITIVE = 4,
  /** Short unit name does not have space between it and preceding number. */
  B_UNIT_DEF_NO_SPACE = 8,
};

/* Define a single unit system. */
struct bUnitCollection {
  const bUnitDef *units;
  /** Basic unit index (when user doesn't specify unit explicitly). */
  int base_unit;
  /** Options for this system. */
  int flag;
  /** To quickly find the last item. */
  int length;
};

/* Keep table Alignment. */
/* clang-format off */


#define UNIT_COLLECTION_LENGTH(def) (ARRAY_SIZE(def) - 1)
#define NULL_UNIT {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0.0, 0.0}


/* Dummy */
static   bUnitDef buDummyDef[] = { {"", nullptr, "", nullptr, nullptr, nullptr, 1.0, 0.0}, NULL_UNIT};
static   bUnitCollection buDummyCollection = {buDummyDef, 0, 0, sizeof(buDummyDef)};


/* Lengths. */
static   bUnitDef buMetricLenDef[] = {
{"kilometer",  "kilometers",  "km",  nullptr, "Kilometers",     "KILOMETERS",  UN_SC_KM,  0.0, B_UNIT_DEF_NONE}, {"hectometer", "hectometers", "hm",  nullptr, "100 Meters",     "HECTOMETERS", UN_SC_HM,  0.0, B_UNIT_DEF_SUPPRESS}, {"dekameter",  "dekameters",  "dam", nullptr, "10 Meters",      "DEKAMETERS",  UN_SC_DAM, 0.0, B_UNIT_DEF_SUPPRESS}, {"meter",      "meters",      "m",   nullptr, "Meters",         "METERS",      UN_SC_M,   0.0, B_UNIT_DEF_NONE},     /* Base unit. */
{"decimeter",  "decimeters",  "dm",  nullptr, "10 Centimeters", "DECIMETERS",  UN_SC_DM,  0.0, B_UNIT_DEF_SUPPRESS}, {"centimeter", "centimeters", "cm",  nullptr, "Centimeters",    "CENTIMETERS", UN_SC_CM,  0.0, B_UNIT_DEF_NONE}, {"millimeter", "millimeters", "mm",  nullptr, "Millimeters",    "MILLIMETERS", UN_SC_MM,  0.0, B_UNIT_DEF_NONE | B_UNIT_DEF_TENTH}, {"micrometer", "micrometers", "µm",  "um", "Micrometers",    "MICROMETERS", UN_SC_UM,  0.0, B_UNIT_DEF_NONE},

/* These get displayed because of float precision problems in the transform header,
 * could work around, but for now probably people won't use these. */
#if 0
{"nanometer", "Nanometers",     "nm", nullptr, 0.000000001, 0.0,   B_UNIT_DEF_NONE}, {"picometer", "Picometers",     "pm", nullptr, 0.000000000001, 0.0, B_UNIT_DEF_NONE},
#endif
NULL_UNIT, };
static const   bUnitCollection buMetricLenCollection = {buMetricLenDef, 3, 0, UNIT_COLLECTION_LENGTH(buMetricLenDef)};


static   bUnitDef buImperialLenDef[] = {
{"mile",    "miles",    "mi",   nullptr, "Miles",    "MILES",    UN_SC_MI,  0.0, B_UNIT_DEF_NONE}, {"furlong", "furlongs", "fur",  nullptr, "Furlongs", "FURLONGS", UN_SC_FUR, 0.0, B_UNIT_DEF_SUPPRESS}, {"chain",   "chains",   "ch",   nullptr, "Chains",   "CHAINS",   UN_SC_CH,  0.0, B_UNIT_DEF_SUPPRESS}, {"yard",    "yards",    "yd",   nullptr, "Yards",    "YARDS",    UN_SC_YD,  0.0, B_UNIT_DEF_SUPPRESS}, {"foot",    "feet",     "'",    "ft", "Feet",     "FEET",     UN_SC_FT,  0.0, B_UNIT_DEF_NONE | B_UNIT_DEF_NO_SPACE}, /* Base unit. */
{"inch",    "inches",   "\"",   "in", "Inches",   "INCHES",   UN_SC_IN,  0.0, B_UNIT_DEF_NONE | B_UNIT_DEF_NO_SPACE}, {"thou",    "thou",     "thou", "mil", "Thou",    "THOU",     UN_SC_MIL, 0.0, B_UNIT_DEF_NONE}, /* Plural for "thou" has no 's'. */
NULL_UNIT, };
static   bUnitCollection buImperialLenCollection = {buImperialLenDef, 4, 0, UNIT_COLLECTION_LENGTH(buImperialLenDef)};


/* Areas. */
static   bUnitDef buMetricAreaDef[] = {
{"square kilometer",  "square kilometers",  "km²",  "km2",  "Square Kilometers",  nullptr, UN_SC_KM * UN_SC_KM,   0.0, B_UNIT_DEF_NONE}, {"square hectometer", "square hectometers", "hm²",  "hm2",  "Square Hectometers", nullptr, UN_SC_HM * UN_SC_HM,   0.0, B_UNIT_DEF_SUPPRESS},   /* Hectare. */
{"square dekameter",  "square dekameters",  "dam²", "dam2", "Square Dekameters",  nullptr, UN_SC_DAM * UN_SC_DAM, 0.0, B_UNIT_DEF_SUPPRESS},  /* are */
{"square meter",      "square meters",      "m²",   "m2",   "Square Meters",      nullptr, UN_SC_M * UN_SC_M,     0.0, B_UNIT_DEF_NONE},   /* Base unit. */
{"square decimeter",  "square decimetees",  "dm²",  "dm2",  "Square Decimeters",  nullptr, UN_SC_DM * UN_SC_DM,   0.0, B_UNIT_DEF_SUPPRESS}, {"square centimeter", "square centimeters", "cm²",  "cm2",  "Square Centimeters", nullptr, UN_SC_CM * UN_SC_CM,   0.0, B_UNIT_DEF_NONE}, {"square millimeter", "square millimeters", "mm²",  "mm2",  "Square Millimeters", nullptr, UN_SC_MM * UN_SC_MM,   0.0, B_UNIT_DEF_NONE | B_UNIT_DEF_TENTH}, {"square micrometer", "square micrometers", "µm²",  "um2",  "Square Micrometers", nullptr, UN_SC_UM * UN_SC_UM,   0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buMetricAreaCollection = {buMetricAreaDef, 3, 0, UNIT_COLLECTION_LENGTH(buMetricAreaDef)};


static   bUnitDef buImperialAreaDef[] = {
{"square mile",    "square miles",    "sq mi", "sq m", "Square Miles",    nullptr, UN_SC_MI * UN_SC_MI,   0.0, B_UNIT_DEF_NONE}, {"square furlong", "square furlongs", "sq fur", nullptr,  "Square Furlongs", nullptr, UN_SC_FUR * UN_SC_FUR, 0.0, B_UNIT_DEF_SUPPRESS}, {"square chain",   "square chains",   "sq ch",  nullptr,  "Square Chains",   nullptr, UN_SC_CH * UN_SC_CH,   0.0, B_UNIT_DEF_SUPPRESS}, {"square yard",    "square yards",    "sq yd",  nullptr,  "Square Yards",    nullptr, UN_SC_YD * UN_SC_YD,   0.0, B_UNIT_DEF_NONE}, {"square foot",    "square feet",     "sq ft",  nullptr,  "Square Feet",     nullptr, UN_SC_FT * UN_SC_FT,   0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"square inch",    "square inches",   "sq in",  nullptr,  "Square Inches",   nullptr, UN_SC_IN * UN_SC_IN,   0.0, B_UNIT_DEF_NONE}, {"square thou",    "square thou",     "sq mil", nullptr,  "Square Thou",     nullptr, UN_SC_MIL * UN_SC_MIL, 0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buImperialAreaCollection = {buImperialAreaDef, 4, 0, UNIT_COLLECTION_LENGTH(buImperialAreaDef)};


/* Volumes. */
static   bUnitDef buMetricVolDef[] = {
{"cubic kilometer",  "cubic kilometers",  "km³",  "km3",  "Cubic Kilometers",  nullptr, UN_SC_KM * UN_SC_KM * UN_SC_KM,    0.0, B_UNIT_DEF_NONE}, {"cubic hectometer", "cubic hectometers", "hm³",  "hm3",  "Cubic Hectometers", nullptr, UN_SC_HM * UN_SC_HM * UN_SC_HM,    0.0, B_UNIT_DEF_SUPPRESS}, {"cubic dekameter",  "cubic dekameters",  "dam³", "dam3", "Cubic Dekameters",  nullptr, UN_SC_DAM * UN_SC_DAM * UN_SC_DAM, 0.0, B_UNIT_DEF_SUPPRESS}, {"cubic meter",      "cubic meters",      "m³",   "m3",   "Cubic Meters",      nullptr, UN_SC_M * UN_SC_M * UN_SC_M,       0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"cubic decimeter",  "cubic decimeters",  "dm³",  "dm3",  "Cubic Decimeters",  nullptr, UN_SC_DM * UN_SC_DM * UN_SC_DM,    0.0, B_UNIT_DEF_SUPPRESS}, {"cubic centimeter", "cubic centimeters", "cm³",  "cm3",  "Cubic Centimeters", nullptr, UN_SC_CM * UN_SC_CM * UN_SC_CM,    0.0, B_UNIT_DEF_NONE}, {"cubic millimeter", "cubic millimeters", "mm³",  "mm3",  "Cubic Millimeters", nullptr, UN_SC_MM * UN_SC_MM * UN_SC_MM,    0.0, B_UNIT_DEF_NONE | B_UNIT_DEF_TENTH}, {"cubic micrometer", "cubic micrometers", "µm³",  "um3",  "Cubic Micrometers", nullptr, UN_SC_UM * UN_SC_UM * UN_SC_UM,    0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buMetricVolCollection = {buMetricVolDef, 3, 0, UNIT_COLLECTION_LENGTH(buMetricVolDef)};


static   bUnitDef buImperialVolDef[] = {
{"cubic mile",    "cubic miles",    "cu mi",  "cu m", "Cubic Miles",    nullptr, UN_SC_MI * UN_SC_MI * UN_SC_MI,    0.0, B_UNIT_DEF_NONE}, {"cubic furlong", "cubic furlongs", "cu fur", nullptr,   "Cubic Furlongs", nullptr, UN_SC_FUR * UN_SC_FUR * UN_SC_FUR, 0.0, B_UNIT_DEF_SUPPRESS}, {"cubic chain",   "cubic chains",   "cu ch",  nullptr,   "Cubic Chains",   nullptr, UN_SC_CH * UN_SC_CH * UN_SC_CH,    0.0, B_UNIT_DEF_SUPPRESS}, {"cubic yard",    "cubic yards",    "cu yd",  nullptr,   "Cubic Yards",    nullptr, UN_SC_YD * UN_SC_YD * UN_SC_YD,    0.0, B_UNIT_DEF_NONE}, {"cubic foot",    "cubic feet",     "cu ft",  nullptr,   "Cubic Feet",     nullptr, UN_SC_FT * UN_SC_FT * UN_SC_FT,    0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"cubic inch",    "cubic inches",   "cu in",  nullptr,   "Cubic Inches",   nullptr, UN_SC_IN * UN_SC_IN * UN_SC_IN,    0.0, B_UNIT_DEF_NONE}, {"cubic thou",    "cubic thou",     "cu mil", nullptr,   "Cubic Thou",     nullptr, UN_SC_MIL * UN_SC_MIL * UN_SC_MIL, 0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buImperialVolCollection = {buImperialVolDef, 4, 0, UNIT_COLLECTION_LENGTH(buImperialVolDef)};


/* Mass. */
static   bUnitDef buMetricMassDef[] = {
{"ton",       "tonnes",     "ton", "t",  "Tonnes",        "TONNES",     UN_SC_MTON, 0.0, B_UNIT_DEF_NONE}, {"quintal",   "quintals",   "ql",  "q",  "100 Kilograms", "QUINTALS",   UN_SC_QL,   0.0, B_UNIT_DEF_SUPPRESS}, {"kilogram",  "kilograms",  "kg",  nullptr, "Kilograms",     "KILOGRAMS",  UN_SC_KG,   0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"hectogram", "hectograms", "hg",  nullptr, "Hectograms",    "HECTOGRAMS", UN_SC_HG,   0.0, B_UNIT_DEF_SUPPRESS}, {"dekagram",  "dekagrams",  "dag", nullptr, "10 Grams",      "DEKAGRAMS",  UN_SC_DAG,  0.0, B_UNIT_DEF_SUPPRESS}, {"gram",      "grams",      "g",   nullptr, "Grams",         "GRAMS",      UN_SC_G,    0.0, B_UNIT_DEF_NONE}, {"milligram", "milligrams", "mg",  nullptr, "Milligrams",    "MILLIGRAMS", UN_SC_MG,   0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buMetricMassCollection = {buMetricMassDef, 2, 0, UNIT_COLLECTION_LENGTH(buMetricMassDef)};


static   bUnitDef buImperialMassDef[] = {
{"ton",           "tonnes",         "ton", "t",  "Tonnes",         "TONNES",         UN_SC_ITON, 0.0, B_UNIT_DEF_NONE}, {"centum weight", "centum weights", "cwt", nullptr, "Centum weights", "CENTUM_WEIGHTS", UN_SC_CWT,  0.0, B_UNIT_DEF_NONE}, {"stone",         "stones",         "st",  nullptr, "Stones",         "STONES",         UN_SC_ST,   0.0, B_UNIT_DEF_NONE}, {"pound",         "pounds",         "lb",  nullptr, "Pounds",         "POUNDS",         UN_SC_LB,   0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"ounce",         "ounces",         "oz",  nullptr, "Ounces",         "OUNCES",         UN_SC_OZ,   0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buImperialMassCollection = {buImperialMassDef, 3, 0, UNIT_COLLECTION_LENGTH(buImperialMassDef)};

/* Even if user scales the system to a point where km^3 is used, velocity and
 * acceleration aren't scaled: that's why we have so few units for them. */

/* Velocity. */
static   bUnitDef buMetricVelDef[] = {
{"meter per second",   "meters per second",   "m/s",  nullptr, "Meters per second",   nullptr, UN_SC_M,            0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"kilometer per hour", "kilometers per hour", "km/h", nullptr, "Kilometers per hour", nullptr, UN_SC_KM / 3600.0f, 0.0, B_UNIT_DEF_SUPPRESS}, NULL_UNIT, };
static   bUnitCollection buMetricVelCollection = {buMetricVelDef, 0, 0, UNIT_COLLECTION_LENGTH(buMetricVelDef)};


static   bUnitDef buImperialVelDef[] = {
{"foot per second", "feet per second", "ft/s", "fps", "Feet per second", nullptr, UN_SC_FT,           0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"mile per hour",   "miles per hour",  "mph",  nullptr,  "Miles per hour",  nullptr, UN_SC_MI / 3600.0f, 0.0, B_UNIT_DEF_SUPPRESS}, NULL_UNIT, };
static   bUnitCollection buImperialVelCollection = {buImperialVelDef, 0, 0, UNIT_COLLECTION_LENGTH(buImperialVelDef)};


/* Acceleration. */
static   bUnitDef buMetricAclDef[] = {
{"meter per second squared", "meters per second squared", "m/s²", "m/s2", "Meters per second squared", nullptr, UN_SC_M, 0.0, B_UNIT_DEF_NONE}, /* Base unit. */
NULL_UNIT, };
static   bUnitCollection buMetricAclCollection = {buMetricAclDef, 0, 0, UNIT_COLLECTION_LENGTH(buMetricAclDef)};


static   bUnitDef buImperialAclDef[] = {
{"foot per second squared", "feet per second squared", "ft/s²", "ft/s2", "Feet per second squared", nullptr, UN_SC_FT, 0.0, B_UNIT_DEF_NONE}, /* Base unit. */
NULL_UNIT, };
static   bUnitCollection buImperialAclCollection = {buImperialAclDef, 0, 0, UNIT_COLLECTION_LENGTH(buImperialAclDef)};


/* Time. */
static   bUnitDef buNaturalTimeDef[] = {
/* Weeks? - probably not needed for Blender. */
{"day",         "days",         "d",   nullptr, "Days",         "DAYS",     86400.0,      0.0, B_UNIT_DEF_NONE}, {"hour",        "hours",        "hr",  "h",  "Hours",        "HOURS",     3600.0,      0.0, B_UNIT_DEF_NONE}, {"minute",      "minutes",      "min", "m",  "Minutes",      "MINUTES",     60.0,      0.0, B_UNIT_DEF_NONE}, {"second",      "seconds",      "sec", "s",  "Seconds",      "SECONDS",      1.0,      0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"millisecond", "milliseconds", "ms",  nullptr, "Milliseconds", "MILLISECONDS", 0.001,    0.0, B_UNIT_DEF_NONE}, {"microsecond", "microseconds", "µs",  "us", "Microseconds", "MICROSECONDS", 0.000001, 0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buNaturalTimeCollection = {buNaturalTimeDef, 3, 0, UNIT_COLLECTION_LENGTH(buNaturalTimeDef)};




static   bUnitDef buNaturalRotDef[] = {
  {"degree",    "degrees",     BLI_STR_UTF8_DEGREE_SIGN,  "d",   "Degrees",    "DEGREES",    M_PI / 180.0,             0.0,  B_UNIT_DEF_NONE | B_UNIT_DEF_NO_SPACE},
  /* arcminutes/arcseconds are used in Astronomy/Navigation areas... */
  {"arcminute", "arcminutes",  "'",  nullptr,  "Arcminutes", "ARCMINUTES", (M_PI / 180.0) / 60.0,    0.0,  B_UNIT_DEF_SUPPRESS | B_UNIT_DEF_NO_SPACE},
  {"arcsecond", "arcseconds",  "\"", nullptr,  "Arcseconds", "ARCSECONDS", (M_PI / 180.0) / 3600.0,  0.0,  B_UNIT_DEF_SUPPRESS | B_UNIT_DEF_NO_SPACE},
  {"radian",    "radians",     "r",  nullptr,  "Radians",    "RADIANS",    1.0,                      0.0,  B_UNIT_DEF_NONE},
#if 0
  {"turn",    "turns",         "t",  nullptr,  "Turns",      nullptr,         1.0 / (M_PI * 2.0),       0.0,  B_UNIT_DEF_NONE},
#endif
NULL_UNIT, };
static   bUnitCollection buNaturalRotCollection = {buNaturalRotDef, 0, 0, UNIT_COLLECTION_LENGTH(buNaturalRotDef)};


/* Camera Lengths. */
static   bUnitDef buCameraLenDef[] = {
{"meter",      "meters",      "m",   nullptr, "Meters",         nullptr, UN_SC_KM,  0.0, B_UNIT_DEF_NONE},     /* Base unit. */
{"decimeter",  "decimeters",  "dm",  nullptr, "10 Centimeters", nullptr, UN_SC_HM,  0.0, B_UNIT_DEF_SUPPRESS}, {"centimeter", "centimeters", "cm",  nullptr, "Centimeters",    nullptr, UN_SC_DAM, 0.0, B_UNIT_DEF_SUPPRESS}, {"millimeter", "millimeters", "mm",  nullptr, "Millimeters",    nullptr, UN_SC_M,   0.0, B_UNIT_DEF_NONE}, {"micrometer", "micrometers", "µm",  "um",  "Micrometers",    nullptr, UN_SC_MM,  0.0, B_UNIT_DEF_SUPPRESS}, NULL_UNIT, };
static   bUnitCollection buCameraLenCollection = {buCameraLenDef, 3, 0, UNIT_COLLECTION_LENGTH(buCameraLenDef)};


/* (Light) Power. */
static   bUnitDef buPowerDef[] = {
{"gigawatt",  "gigawatts",  "GW", nullptr, "Gigawatts",  nullptr, 1e9f,  0.0, B_UNIT_DEF_NONE}, {"megawatt",  "megawatts",  "MW", nullptr, "Megawatts",  nullptr, 1e6f,  0.0, B_UNIT_DEF_CASE_SENSITIVE}, {"kilowatt",  "kilowatts",  "kW", nullptr, "Kilowatts",  nullptr, 1e3f,  0.0, B_UNIT_DEF_SUPPRESS}, {"watt",      "watts",      "W",  nullptr, "Watts",      nullptr, 1.0f,  0.0, B_UNIT_DEF_NONE}, /* Base unit. */
{"milliwatt", "milliwatts", "mW", nullptr, "Milliwatts", nullptr, 1e-3f, 0.0, B_UNIT_DEF_CASE_SENSITIVE}, {"microwatt", "microwatts", "µW", "uW", "Microwatts", nullptr, 1e-6f, 0.0, B_UNIT_DEF_NONE}, {"nanowatt",  "nanowatts",  "nW", nullptr, "Nanowatts",  nullptr, 1e-9f, 0.0, B_UNIT_DEF_NONE}, NULL_UNIT, };
static   bUnitCollection buPowerCollection = {buPowerDef, 3, 0, UNIT_COLLECTION_LENGTH(buPowerDef)};


/* Temperature */
static   bUnitDef buMetricTempDef[] = {
  {"kelvin",  "kelvin",  "K",  nullptr, "Kelvin",  "KELVIN",  1.0f, 0.0,    B_UNIT_DEF_NONE}, /* Base unit. */
  {"celsius", "celsius", BLI_STR_UTF8_DEGREE_SIGN "C", "C",  "Celsius", "CELSIUS", 1.0f, 273.15, B_UNIT_DEF_NONE},
  NULL_UNIT,
};
static   bUnitCollection buMetricTempCollection = {buMetricTempDef, 0, 0, UNIT_COLLECTION_LENGTH(buMetricTempDef)};


static   bUnitDef buImperialTempDef[] = {
  {"kelvin",     "kelvin",     "K",  nullptr, "Kelvin",     "KELVIN",     1.0f,      0.0,    B_UNIT_DEF_NONE}, /* Base unit. */
  {"fahrenheit", "fahrenheit", BLI_STR_UTF8_DEGREE_SIGN "F", "F",  "Fahrenheit", "FAHRENHEIT", UN_SC_FAH, 459.67, B_UNIT_DEF_NONE},
  NULL_UNIT,
};
static   bUnitCollection buImperialTempCollection = {
buImperialTempDef, 1, 0, UNIT_COLLECTION_LENGTH(buImperialTempDef)};

/* clang-format on */

#define UNIT_SYSTEM_TOT (((sizeof(bUnitSystems) / B_UNIT_TYPE_TOT) / sizeof(void *)) - 1)
static const bUnitCollection *bUnitSystems[][B_UNIT_TYPE_TOT] = {
    /* Natural. */
    {nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     &buNaturalRotCollection,
     &buNaturalTimeCollection,
     &buNaturalTimeCollection,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
    /* Metric. */
    {nullptr,
     &buMetricLenCollection,
     &buMetricAreaCollection,
     &buMetricVolCollection,
     &buMetricMassCollection,
     &buNaturalRotCollection,
     &buNaturalTimeCollection,
     &buNaturalTimeCollection,
     &buMetricVelCollection,
     &buMetricAclCollection,
     &buCameraLenCollection,
     &buPowerCollection,
     &buMetricTempCollection},
    /* Imperial. */
    {nullptr,
     &buImperialLenCollection,
     &buImperialAreaCollection,
     &buImperialVolCollection,
     &buImperialMassCollection,
     &buNaturalRotCollection,
     &buNaturalTimeCollection,
     &buNaturalTimeCollection,
     &buImperialVelCollection,
     &buImperialAclCollection,
     &buCameraLenCollection,
     &buPowerCollection,
     &buImperialTempCollection},
    {nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     nullptr},
};

static const bUnitCollection *unit_get_system(int system, int type)
{
  BLI_assert((system > -1) && (system < UNIT_SYSTEM_TOT) && (type > -1) &&
             (type < B_UNIT_TYPE_TOT));
  return bUnitSystems[system][type]; /* Select system to use: metric/imperial/other? */
}

static const bUnitDef *unit_default(const bUnitCollection *usys)
{
  return &usys->units[usys->base_unit];
}

static const bUnitDef *unit_best_fit(double value,
                                     const bUnitCollection *usys,
                                     const bUnitDef *unit_start,
                                     int suppress)
{
  double value_abs = value > 0.0 ? value : -value;

  for (const bUnitDef *unit = unit_start ? unit_start : usys->units; unit->name; unit++) {

    if (suppress && (unit->flag & B_UNIT_DEF_SUPPRESS)) {
      continue;
    }

    /* Scale down scalar so 1cm doesn't convert to 10mm because of float error. */
    if (UNLIKELY(unit->flag & B_UNIT_DEF_TENTH)) {
      if (value_abs >= unit->scalar * (0.1 - EPS)) {
        return unit;
      }
    }
    else {
      if (value_abs >= unit->scalar * (1.0 - EPS)) {
        return unit;
      }
    }
  }

  return unit_default(usys);
}

/* Convert into 2 units and 2 values for "2ft, 3inch" syntax. */
static void unit_dual_convert(double value,
                              const bUnitCollection *usys,
                              bUnitDef const **r_unit_a,
                              bUnitDef const **r_unit_b,
                              double *r_value_a,
                              double *r_value_b,
                              const bUnitDef *main_unit)
{
  const bUnitDef *unit = (main_unit) ? main_unit : unit_best_fit(value, usys, nullptr, 1);

  const double scaled_value = value / unit->scalar;
  *r_value_a = (value < 0.0 ? ceil(scaled_value) : floor(scaled_value)) * unit->scalar;
  *r_value_b = value - (*r_value_a);

  *r_unit_a = unit;
  *r_unit_b = unit_best_fit(*r_value_b, usys, *r_unit_a, 1);
}

static size_t unit_as_string(char *str,
                             int str_maxncpy,
                             double value,
                             int prec,
                             const bUnitCollection *usys,
                             /* Non exposed options. */
                             const bUnitDef *unit,
                             char pad)
{
  if (unit == nullptr) {
    if (value == 0.0) {
      /* Use the default units since there is no way to convert. */
      unit = unit_default(usys);
    }
    else {
      unit = unit_best_fit(value, usys, nullptr, 1);
    }
  }

  double value_conv = (value / unit->scalar) - unit->bias;
  bool strip_skip = false;

  /* Negative precision is used to disable stripping of zeroes.
   * This reduces text jumping when changing values. */
  if (prec < 0) {
    strip_skip = true;
    prec *= -1;
  }

  /* Adjust precision to expected number of significant digits.
   * Note that here, we shall not have to worry about very big/small numbers, units are expected
   * to replace 'scientific notation' in those cases. */
  prec -= integer_digits_d(value_conv);

  CLAMP(prec, 0, 6);

  /* Convert to a string. */
  size_t len = BLI_snprintf_rlen(str, str_maxncpy, "%.*f", prec, value_conv);

  /* Add unit prefix and strip zeros. */

  /* Replace trailing zero's with spaces so the number
   * is less complicated but alignment in a button won't
   * jump about while dragging. */
  size_t i = len - 1;

  if (prec > 0) {
    if (!strip_skip) {
      while (i > 0 && str[i] == '0') { /* 4.300 -> 4.3 */
        str[i--] = pad;
      }

      if (i > 0 && str[i] == '.') { /* 10. -> 10 */
        str[i--] = pad;
      }
    }
  }

  /* Now add a space for all units except foot, inch, degree, arcminute, arcsecond. */
  if (!(unit->flag & B_UNIT_DEF_NO_SPACE)) {
    str[++i] = ' ';
  }

  /* Now add the suffix. */
  if (i < str_maxncpy) {
    int j = 0;
    i++;
    while (unit->name_short[j] && (i < str_maxncpy)) {
      str[i++] = unit->name_short[j++];
    }
  }

  /* Terminate no matter what's done with padding above. */
  if (i >= str_maxncpy) {
    i = str_maxncpy - 1;
  }

  str[i] = '\0';
  return i;
}

static bool unit_should_be_split(int type)
{
  return ELEM(type, B_UNIT_LENGTH, B_UNIT_MASS, B_UNIT_TIME, B_UNIT_CAMERA);
}

struct PreferredUnits {
  int system;
  int rotation;
  /* USER_UNIT_ADAPTIVE means none, otherwise the value is the index in the collection. */
  int length;
  int mass;
  int time;
  int temperature;
};

static PreferredUnits preferred_units_from_UnitSettings(const UnitSettings *settings)
{
  PreferredUnits units = {0};
  units.system = settings->system;
  units.rotation = settings->system_rotation;
  units.length = settings->length_unit;
  units.mass = settings->mass_unit;
  units.time = settings->time_unit;
  units.temperature = settings->temperature_unit;
  return units;
}

static size_t unit_as_string_split_pair(char *str,
                                        int str_maxncpy,
                                        double value,
                                        int prec,
                                        const bUnitCollection *usys,
                                        const bUnitDef *main_unit)
{
  const bUnitDef *unit_a, *unit_b;
  double value_a, value_b;
  unit_dual_convert(value, usys, &unit_a, &unit_b, &value_a, &value_b, main_unit);

  /* Check the 2 is a smaller unit. */
  if (unit_b > unit_a) {
    size_t i = unit_as_string(str, str_maxncpy, value_a, prec, usys, unit_a, '\0');

    prec -= integer_digits_d(value_a / unit_b->scalar) -
            integer_digits_d(value_b / unit_b->scalar);
    prec = max_ii(prec, 0);

    /* Is there enough space for at least 1 char of the next unit? */
    if (i + 2 < str_maxncpy) {
      str[i++] = ' ';

      /* Use low precision since this is a smaller unit. */
      i += unit_as_string(str + i, str_maxncpy - i, value_b, prec, usys, unit_b, '\0');
    }
    return i;
  }

  return -1;
}

static bool is_valid_unit_collection(const bUnitCollection *usys)
{
  return usys != nullptr && usys->units[0].name != nullptr;
}

static const bUnitDef *get_preferred_display_unit_if_used(int type, PreferredUnits units)
{
  const bUnitCollection *usys = unit_get_system(units.system, type);
  if (!is_valid_unit_collection(usys)) {
    return nullptr;
  }

  int max_offset = usys->length - 1;

  switch (type) {
    case B_UNIT_LENGTH:
    case B_UNIT_AREA:
    case B_UNIT_VOLUME:
      if (units.length == USER_UNIT_ADAPTIVE) {
        return nullptr;
      }
      return usys->units + MIN2(units.length, max_offset);
    case B_UNIT_MASS:
      if (units.mass == USER_UNIT_ADAPTIVE) {
        return nullptr;
      }
      return usys->units + MIN2(units.mass, max_offset);
    case B_UNIT_TIME:
      if (units.time == USER_UNIT_ADAPTIVE) {
        return nullptr;
      }
      return usys->units + MIN2(units.time, max_offset);
    case B_UNIT_ROTATION:
      if (units.rotation == 0) {
        return usys->units + 0;
      }
      else if (units.rotation == USER_UNIT_ROT_RADIANS) {
        return usys->units + 3;
      }
      break;
    case B_UNIT_TEMPERATURE:
      if (units.temperature == USER_UNIT_ADAPTIVE) {
        return nullptr;
      }
      return usys->units + MIN2(units.temperature, max_offset);
    default:
      break;
  }
  return nullptr;
}

/* Return the length of the generated string. */
static size_t unit_as_string_main(char *str,
                                  int str_maxncpy,
                                  double value,
                                  int prec,
                                  int type,
                                  bool split,
                                  bool pad,
                                  PreferredUnits units)
{
  const bUnitCollection *usys = unit_get_system(units.system, type);
  const bUnitDef *main_unit = nullptr;

  if (!is_valid_unit_collection(usys)) {
    usys = &buDummyCollection;
  }
  else {
    main_unit = get_preferred_display_unit_if_used(type, units);
  }

  if (split && unit_should_be_split(type)) {
    int length = unit_as_string_split_pair(str, str_maxncpy, value, prec, usys, main_unit);
    /* Failed when length is negative, fallback to no split. */
    if (length >= 0) {
      return length;
    }
  }

  return unit_as_string(str, str_maxncpy, value, prec, usys, main_unit, pad ? ' ' : '\0');
}

size_t BKE_unit_value_as_string_adaptive(
    char *str, int str_maxncpy, double value, int prec, int system, int type, bool split, bool pad)
{
  PreferredUnits units;
  units.system = system;
  units.rotation = 0;
  units.length = USER_UNIT_ADAPTIVE;
  units.mass = USER_UNIT_ADAPTIVE;
  units.time = USER_UNIT_ADAPTIVE;
  units.temperature = USER_UNIT_ADAPTIVE;
  return unit_as_string_main(str, str_maxncpy, value, prec, type, split, pad, units);
}

size_t BKE_unit_value_as_string(char *str,
                                int str_maxncpy,
                                double value,
                                int prec,
                                int type,
                                const UnitSettings *settings,
                                bool pad)
{
  bool do_split = (settings->flag & USER_UNIT_OPT_SPLIT) != 0;
  PreferredUnits units = preferred_units_from_UnitSettings(settings);
  return unit_as_string_main(str, str_maxncpy, value, prec, type, do_split, pad, units);
}

BLI_INLINE bool isalpha_or_utf8(const int ch)
{
  return (ch >= 128 || isalpha(ch));
}

static const char *unit_find_str(const char *str, const char *substr, bool case_sensitive)
{
  if (substr == nullptr || substr[0] == '\0') {
    return nullptr;
  }

  while (true) {
    /* Unit detection is case insensitive. */
    const char *str_found;
    if (case_sensitive) {
      str_found = strstr(str, substr);
    }
    else {
      str_found = BLI_strcasestr(str, substr);
    }

    if (str_found) {
      /* Previous char cannot be a letter. */
      if (str_found == str ||
          /* Weak unicode support!, so "µm" won't match up be replaced by "m"
           * since non ascii utf8 values will NEVER return true */
          isalpha_or_utf8(*BLI_str_find_prev_char_utf8(str_found, str)) == 0)
      {
        /* Next char cannot be alpha-numeric. */
        int len_name = strlen(substr);

        if (!isalpha_or_utf8(*(str_found + len_name))) {
          return str_found;
        }
      }
      /* If str_found is not a valid unit, we have to check further in the string... */
      for (str_found++; isalpha_or_utf8(*str_found); str_found++) {
        /* Pass. */
      }
      str = str_found;
    }
    else {
      break;
    }
  }

  return nullptr;
}

/* Note that numbers are added within brackets.
 * ") " - is used to detect numbers we added so we can detect if commas need to be added.
 *
 * "1m1cm+2mm"              - Original value.
 * "1*1#1*0.01#+2*0.001#"   - Replace numbers.
 * "1*1+1*0.01 +2*0.001 "   - Add plus signs if ( + - * / | & ~ < > ^ ! = % ) not found in between.
 */

/* Not too strict, (+ - * /) are most common. */
static bool ch_is_op(char op)
{
  switch (op) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '|':
    case '&':
    case '~':
    case '<':
    case '>':
    case '^':
    case '!':
    case '=':
    case '%':
      return true;
    default:
      return false;
  }
}

/**
 * Helper function for #unit_distribute_negatives to find the next negative to distribute.
 *
 * \note This unnecessarily skips the next space if it comes right after the "-"
 * just to make a more predictable output.
 */
static const char *find_next_negative(const char *str, const char *remaining_str)
{
  const char *str_found = strstr(remaining_str, "-");

  if (str_found == nullptr) {
    return nullptr;
  }

  /* Don't use the "-" from scientific notation, but make sure we can look backwards first. */
  if ((str_found != str) && ELEM(*(str_found - 1), 'e', 'E')) {
    return find_next_negative(str, str_found + 1);
  }

  if (*(str_found + 1) == ' ') {
    str_found++;
  }

  return str_found + 1;
}

/**
 * Helper function for #unit_distribute_negatives to find the next operation, including "-".
 *
 * \note This unnecessarily skips the space before the operation character
 * just to make a more predictable output.
 */
static char *find_next_op(const char *str, char *remaining_str, int remaining_str_maxncpy)
{
  int i;
  for (i = 0; i < remaining_str_maxncpy; i++) {
    if (remaining_str[i] == '\0') {
      return remaining_str + i;
    }

    if (ch_is_op(remaining_str[i])) {
      /* Make sure we don't look backwards before the start of the string. */
      if (remaining_str != str && i != 0) {
        /* Check for velocity or acceleration (e.g. '/' in 'ft/s' is not an op). */
        if ((remaining_str[i] == '/') && ELEM(remaining_str[i - 1], 't', 'T', 'm', 'M') &&
            ELEM(remaining_str[i + 1], 's', 'S'))
        {
          continue;
        }

        /* Check for scientific notation. */
        if (ELEM(remaining_str[i - 1], 'e', 'E')) {
          continue;
        }

        /* Return position before a space character. */
        if (remaining_str[i - 1] == ' ') {
          i--;
        }
      }

      return remaining_str + i;
    }
  }
  BLI_assert_msg(0, "String should be nullptr terminated");
  return remaining_str + i;
}

/**
 * Put parentheses around blocks of values after negative signs to get rid of an implied "+"
 * between numbers without an operation between them. For example:
 *
 * "-1m50cm + 1 - 2m50cm"  ->  "-(1m50cm) + 1 - (2m50cm)"
 */
static bool unit_distribute_negatives(char *str, const int str_maxncpy)
{
  bool changed = false;

  char *remaining_str = str;
  while ((remaining_str = const_cast<char *>(find_next_negative(str, remaining_str))) != nullptr) {
    int remaining_str_maxncpy;
    /* Exit early in the unlikely situation that we've run out of length to add the parentheses. */
    remaining_str_maxncpy = str_maxncpy - int(remaining_str - str);
    if (remaining_str_maxncpy <= 2) {
      return changed;
    }

    changed = true;

    /* Add '(', shift the following characters to the right to make space. */
    memmove(remaining_str + 1, remaining_str, remaining_str_maxncpy - 2);
    *remaining_str = '(';

    /* Add the ')' before the next operation or at the end. */
    remaining_str = find_next_op(str, remaining_str + 1, remaining_str_maxncpy);
    remaining_str_maxncpy = str_maxncpy - int(remaining_str - str);
    memmove(remaining_str + 1, remaining_str, remaining_str_maxncpy - 2);
    *remaining_str = ')';

    /* Only move forward by 1 even though we added two characters. Minus signs need to be able to
     * apply to the next block of values too. */
    remaining_str += 1;
  }

  return changed;
}

/**
 * Helper for #unit_scale_str for the process of correctly applying the order of operations
 * for the unit's bias term.
 */
static int find_previous_non_value_char(const char *str, const int start_ofs)
{
  for (int i = start_ofs; i > 0; i--) {
    if (ch_is_op(str[i - 1]) || strchr("( )", str[i - 1])) {
      return i;
    }
  }
  return 0;
}

/**
 * Helper for #unit_scale_str for the process of correctly applying the order of operations
 * for the unit's bias term.
 */
static int find_end_of_value_chars(const char *str, const int str_maxncpy, const int start_ofs)
{
  int i;
  for (i = start_ofs; i < str_maxncpy; i++) {
    if (!strchr("0123456789eE.", str[i])) {
      return i;
    }
  }
  return i;
}

static int unit_scale_str(char *str,
                          int str_maxncpy,
                          char *str_tmp,
                          double scale_pref,
                          const bUnitDef *unit,
                          const char *replace_str,
                          bool case_sensitive)
{
  if (str_maxncpy < 0) {
    return 0;
  }

  /* XXX: investigate, does not respect str_maxncpy properly. */
  char *str_found = (char *)unit_find_str(str, replace_str, case_sensitive);

  if (str_found == nullptr) {
    return 0;
  }

  int found_ofs = int(str_found - str);

  int len = strlen(str);

  /* Deal with unit bias for temperature units. Order of operations is important, so we
   * have to add parentheses, add the bias, then multiply by the scalar like usual.
   *
   * NOTE: If these changes don't fit in the buffer properly unit evaluation has failed,
   * just try not to destroy anything while failing. */
  if (unit->bias != 0.0) {
    /* Add the open parenthesis. */
    int prev_op_ofs = find_previous_non_value_char(str, found_ofs);
    if (len + 1 < str_maxncpy) {
      memmove(str + prev_op_ofs + 1, str + prev_op_ofs, len - prev_op_ofs + 1);
      str[prev_op_ofs] = '(';
      len++;
      found_ofs++;
      str_found++;
    } /* If this doesn't fit, we have failed. */

    /* Add the addition sign, the bias, and the close parenthesis after the value. */
    int value_end_ofs = find_end_of_value_chars(str, str_maxncpy, prev_op_ofs + 2);
    int len_bias_num = BLI_snprintf_rlen(str_tmp, TEMP_STR_SIZE, "+%.9g)", unit->bias);
    if (value_end_ofs + len_bias_num < str_maxncpy) {
      memmove(str + value_end_ofs + len_bias_num, str + value_end_ofs, len - value_end_ofs + 1);
      memcpy(str + value_end_ofs, str_tmp, len_bias_num);
      len += len_bias_num;
      found_ofs += len_bias_num;
      str_found += len_bias_num;
    } /* If this doesn't fit, we have failed. */
  }

  int len_name = strlen(replace_str);
  int len_move = (len - (found_ofs + len_name)) + 1; /* 1+ to copy the string terminator. */

  /* "#" Removed later */
  int len_num = BLI_snprintf_rlen(
      str_tmp, TEMP_STR_SIZE, "*%.9g" SEP_STR, unit->scalar / scale_pref);

  if (len_num > str_maxncpy) {
    len_num = str_maxncpy;
  }

  if (found_ofs + len_num + len_move > str_maxncpy) {
    /* Can't move the whole string, move just as much as will fit. */
    len_move -= (found_ofs + len_num + len_move) - str_maxncpy;
  }

  if (len_move > 0) {
    /* Resize the last part of the string.
     * May grow or shrink the string. */
    memmove(str_found + len_num, str_found + len_name, len_move);
  }

  if (found_ofs + len_num > str_maxncpy) {
    /* Not even the number will fit into the string, only copy part of it. */
    len_num -= (found_ofs + len_num) - str_maxncpy;
  }

  if (len_num > 0) {
    /* It's possible none of the number could be copied in. */
    memcpy(str_found, str_tmp, len_num); /* Without the string terminator. */
  }

  /* Since the null terminator won't be moved if the stringlen_max
   * was not long enough to fit everything in it. */
  str[str_maxncpy - 1] = '\0';
  return found_ofs + len_num;
}

static int unit_replace(
    char *str, int str_maxncpy, char *str_tmp, double scale_pref, const bUnitDef *unit)
{
  const bool case_sensitive = (unit->flag & B_UNIT_DEF_CASE_SENSITIVE) != 0;
  int ofs = 0;
  ofs += unit_scale_str(
      str + ofs, str_maxncpy - ofs, str_tmp, scale_pref, unit, unit->name_short, case_sensitive);
  ofs += unit_scale_str(
      str + ofs, str_maxncpy - ofs, str_tmp, scale_pref, unit, unit->name_plural, false);
  ofs += unit_scale_str(
      str + ofs, str_maxncpy - ofs, str_tmp, scale_pref, unit, unit->name_alt, case_sensitive);
  ofs += unit_scale_str(
      str + ofs, str_maxncpy - ofs, str_tmp, scale_pref, unit, unit->name, false);
  return ofs;
}

static bool unit_find(const char *str, const bUnitDef *unit)
{
  const bool case_sensitive = (unit->flag & B_UNIT_DEF_CASE_SENSITIVE) != 0;
  if (unit_find_str(str, unit->name_short, case_sensitive)) {
    return true;
  }
  if (unit_find_str(str, unit->name_plural, false)) {
    return true;
  }
  if (unit_find_str(str, unit->name_alt, case_sensitive)) {
    return true;
  }
  if (unit_find_str(str, unit->name, false)) {
    return true;
  }

  return false;
}

static const bUnitDef *unit_find_in_collection(const bUnitCollection *usys, const char *str)
{
  for (const bUnitDef *unit = usys->units; unit->name; unit++) {
    if (unit_find(str, unit)) {
      return unit;
    }
  }
  return nullptr;
}

/**
 * Try to find a default unit from current or previous string.
 * This allows us to handle cases like 2 + 2mm, people would expect to get 4mm, not 2.002m!
 * \note This does not handle corner cases like 2 + 2cm + 1 + 2.5mm... We can't support
 * everything.
 */
static const bUnitDef *unit_detect_from_str(const bUnitCollection *usys,
                                            const char *str,
                                            const char *str_prev)
{
  /* See which units the new value has. */
  const bUnitDef *unit = unit_find_in_collection(usys, str);
  /* Else, try to infer the default unit from the previous string. */
  if (str_prev && (unit == nullptr)) {
    /* See which units the original value had. */
    unit = unit_find_in_collection(usys, str_prev);
  }
  /* Else, fall back to default unit. */
  if (unit == nullptr) {
    unit = unit_default(usys);
  }

  return unit;
}

bool BKE_unit_string_contains_unit(const char *str, int type)
{
  for (int system = 0; system < UNIT_SYSTEM_TOT; system++) {
    const bUnitCollection *usys = unit_get_system(system, type);
    if (!is_valid_unit_collection(usys)) {
      continue;
    }
    if (unit_find_in_collection(usys, str)) {
      return true;
    }
  }
  return false;
}

double BKE_unit_apply_preferred_unit(const UnitSettings *settings, int type, double value)
{
  PreferredUnits units = preferred_units_from_UnitSettings(settings);
  const bUnitDef *unit = get_preferred_display_unit_if_used(type, units);

  const double scalar = (unit == nullptr) ? BKE_unit_base_scalar(units.system, type) :
                                            unit->scalar;
  const double bias = (unit == nullptr) ? 0.0 : unit->bias; /* Base unit shouldn't have a bias. */

  return value * scalar + bias;
}

bool BKE_unit_replace_string(
    char *str, int str_maxncpy, const char *str_prev, double scale_pref, int system, int type)
{
  const bUnitCollection *usys = unit_get_system(system, type);
  if (!is_valid_unit_collection(usys)) {
    return false;
  }

  double scale_pref_base = scale_pref;
  char str_tmp[TEMP_STR_SIZE];
  bool changed = false;

  /* Fix cases like "-1m50cm" which would evaluate to -0.5m without this. */
  changed |= unit_distribute_negatives(str, str_maxncpy);

  /* Try to find a default unit from current or previous string. */
  const bUnitDef *default_unit = unit_detect_from_str(usys, str, str_prev);

  /* We apply the default unit to the whole expression (default unit is now the reference
   * '1.0' one). */
  scale_pref_base *= default_unit->scalar;

  /* Apply the default unit on the whole expression, this allows to handle nasty cases like
   * '2+2in'. */
  if (SNPRINTF(str_tmp, "(%s)*%.9g", str, default_unit->scalar) < sizeof(str_tmp)) {
    BLI_strncpy(str, str_tmp, str_maxncpy);
  }
  else {
    /* BLI_snprintf would not fit into str_tmp, can't do much in this case.
     * Check for this because otherwise BKE_unit_replace_string could call itself forever. */
    return changed;
  }

  for (const bUnitDef *unit = usys->units; unit->name; unit++) {
    /* In case there are multiple instances. */
    while (unit_replace(str, str_maxncpy, str_tmp, scale_pref_base, unit)) {
      changed = true;
    }
  }

  /* Try other unit systems now, so we can evaluate imperial when metric is set for eg. */
  /* Note that checking other systems at that point means we do not support their units as
   * 'default' one. In other words, when in metrics, typing '2+2in' will give 2 meters 2 inches,
   * not 4 inches. I do think this is the desired behavior!
   */
  for (int system_iter = 0; system_iter < UNIT_SYSTEM_TOT; system_iter++) {
    if (system_iter == system) {
      continue;
    }
    const bUnitCollection *usys_iter = unit_get_system(system_iter, type);
    if (usys_iter == nullptr) {
      continue;
    }

    for (const bUnitDef *unit = usys_iter->units; unit->name; unit++) {
      int ofs = 0;
      /* In case there are multiple instances. */
      while ((ofs = unit_replace(str + ofs, str_maxncpy - ofs, str_tmp, scale_pref_base, unit))) {
        changed = true;
      }
    }
  }

  /* Replace # with add sign when there is no operator between it and the next number.
   *
   * "1*1# 3*100# * 3"  ->  "1*1+ 3*100  * 3"
   */
  {
    char *str_found = str;

    while ((str_found = strchr(str_found, SEP_CHR))) {
      bool op_found = false;

      /* Any operators after this? */
      for (const char *ch = str_found + 1; *ch != '\0'; ch++) {
        if (ELEM(*ch, ' ', '\t')) {
          continue;
        }
        op_found = (ch_is_op(*ch) || ELEM(*ch, ',', ')'));
        break;
      }

      /* If found an op, comma or closing parenthesis, no need to insert a '+', else we need it. */
      *str_found++ = op_found ? ' ' : '+';
    }
  }

  return changed;
}

void BKE_unit_name_to_alt(char *str, int str_maxncpy, const char *orig_str, int system, int type)
{
  const bUnitCollection *usys = unit_get_system(system, type);

  /* Find and substitute all units. */
  for (const bUnitDef *unit = usys->units; unit->name && (str_maxncpy > 0); unit++) {
    if (unit->name_alt == nullptr) {
      continue;
    }
    const bool case_sensitive = (unit->flag & B_UNIT_DEF_CASE_SENSITIVE) != 0;
    const char *found = unit_find_str(orig_str, unit->name_short, case_sensitive);
    if (found == nullptr) {
      continue;
    }

    int offset = int(found - orig_str);

    /* Copy everything before the unit. */
    if (offset < str_maxncpy) {
      memcpy(str, orig_str, offset);
    }
    else {
      BLI_strncpy(str, orig_str, str_maxncpy);
      offset = str_maxncpy;
    }

    str += offset;
    orig_str += offset + strlen(unit->name_short);
    str_maxncpy -= offset;

    /* Print the alt_name. */
    const int len_name = BLI_strncpy_rlen(str, unit->name_alt, str_maxncpy);
    BLI_assert(len_name < str_maxncpy);
    str += len_name;
    str_maxncpy -= len_name;
  }

  /* Finally copy the rest of the string. */
  BLI_strncpy(str, orig_str, str_maxncpy);
}

double BKE_unit_closest_scalar(double value, int system, int type)
{
  const bUnitCollection *usys = unit_get_system(system, type);

  if (usys == nullptr) {
    return -1;
  }

  const bUnitDef *unit = unit_best_fit(value, usys, nullptr, 1);
  if (unit == nullptr) {
    return -1;
  }

  return unit->scalar;
}

double BKE_unit_base_scalar(int system, int type)
{
  const bUnitCollection *usys = unit_get_system(system, type);
  if (usys) {
    return unit_default(usys)->scalar;
  }

  return 1.0;
}

bool BKE_unit_is_valid(int system, int type)
{
  return !(system < 0 || system > UNIT_SYSTEM_TOT || type < 0 || type > B_UNIT_TYPE_TOT);
}

void BKE_unit_system_get(int system, int type, void const **r_usys_pt, int *r_len)
{
  const bUnitCollection *usys = unit_get_system(system, type);
  *r_usys_pt = usys;

  if (usys == nullptr) {
    *r_len = 0;
    return;
  }

  *r_len = usys->length;
}

int BKE_unit_base_get(const void *usys_pt)
{
  return ((bUnitCollection *)usys_pt)->base_unit;
}

int BKE_unit_base_of_type_get(int system, int type)
{
  return unit_get_system(system, type)->base_unit;
}

const char *BKE_unit_name_get(const void *usys_pt, int index)
{
  const bUnitCollection *usys = static_cast<const bUnitCollection *>(usys_pt);
  BLI_assert(uint(index) < uint(usys->length));
  return usys->units[index].name;
}
const char *BKE_unit_display_name_get(const void *usys_pt, int index)
{
  const bUnitCollection *usys = static_cast<const bUnitCollection *>(usys_pt);
  BLI_assert(uint(index) < uint(usys->length));
  return usys->units[index].name_display;
}
const char *BKE_unit_identifier_get(const void *usys_pt, int index)
{
  const bUnitCollection *usys = static_cast<const bUnitCollection *>(usys_pt);
  BLI_assert(uint(index) < uint(usys->length));
  const bUnitDef *unit = &usys->units[index];
  if (unit->identifier == nullptr) {
    BLI_assert_msg(0, "identifier for this unit is not specified yet");
  }
  return unit->identifier;
}

double BKE_unit_scalar_get(const void *usys_pt, int index)
{
  const bUnitCollection *usys = static_cast<const bUnitCollection *>(usys_pt);
  BLI_assert(uint(index) < uint(usys->length));
  return usys->units[index].scalar;
}

bool BKE_unit_is_suppressed(const void *usys_pt, int index)
{
  const bUnitCollection *usys = static_cast<const bUnitCollection *>(usys_pt);
  BLI_assert(uint(index) < uint(usys->length));
  return (usys->units[index].flag & B_UNIT_DEF_SUPPRESS) != 0;
}
