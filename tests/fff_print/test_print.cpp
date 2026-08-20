#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <fstream>
#include <iterator>
#include "nlohmann/json.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Changing the number of solid shell layers does not make all surfaces internal", "[Print]") {
    GIVEN("sliced 20mm cube and config with top_shell_layers = 2 and bottom_shell_layers = 1") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
            { "top_shell_layers",           2 },
            { "bottom_shell_layers",        1 },
            { "layer_height",               0.25 }, // get a known number of layers
            { "initial_layer_print_height", 0.25 }
			});
        Slic3r::Print print;
        Slic3r::Model model;
        Slic3r::Test::init_print({cube(20)}, print, model, config);
        // Precondition: Ensure that the model has 2 solid top layers (79, 78)
        // and one solid bottom layer (0).
		auto test_is_solid_infill = [&print](size_t obj_id, size_t layer_id) {
		    const Layer &layer = *(print.objects().at(obj_id)->get_layer((int)layer_id));
		    // iterate over all of the regions in the layer
		    for (const LayerRegion *region : layer.regions()) {
		        // for each region, iterate over the fill surfaces
		        for (const Surface &surface : region->fill_surfaces.surfaces)
		            CHECK(surface.is_solid());
		    }
		};
        print.process();
        test_is_solid_infill(0,  0); // should be solid
        test_is_solid_infill(0, 79); // should be solid
        test_is_solid_infill(0, 78); // should be solid
        WHEN("Model is re-sliced with top_shell_layers == 3") {
			config.set("top_shell_layers", 3);
			print.apply(model, config);
            print.process();
            THEN("Print object does not have 0 solid bottom layers.") {
                test_is_solid_infill(0, 0);
            }
            AND_THEN("Print object has 3 top solid layers") {
                test_is_solid_infill(0, 79);
                test_is_solid_infill(0, 78);
                test_is_solid_infill(0, 77);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Print::validate() warning collection
//
// validate() returns its warnings in a vector. The warning paths deliberately
// differ in how many entries they produce; these tests pin down each behaviour:
//   * independent checks    -> stack (one entry each)
//   * motion-ability        -> coalesce into one (mutually exclusive, gated)
//   * clumping detection    -> one independent warning
//   * layered clearance     -> many collisions concatenated into one entry
//   * null warnings pointer -> no-op, no crash, no blocking error
// ---------------------------------------------------------------------------
namespace {

// Build `n` 20mm cubes (spread apart, or stacked at the origin when `overlap`) into
// `model`/`print` and apply `config`, leaving the print ready to validate(). No slicing needed.
void build_cubes(Slic3r::Model& model, Slic3r::Print& print,
                 DynamicPrintConfig config, int n, bool overlap)
{
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n")); // validate() relative-E reset

    for (int i = 0; i < n; ++i) {
        ModelObject* object = model.add_object();
        object->add_volume(cube(20));
        ModelInstance* inst = object->add_instance();
        inst->set_offset(Vec3d(overlap ? 0.0 : i * 60.0, 0.0, 0.0));
    }
    for (ModelObject* mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }
    print.apply(model, config);
}

// Build cubes and run validate(), collecting warnings; returns the blocking error.
StringObjectException validate_cubes(const DynamicPrintConfig& config,
                                     std::vector<StringObjectException>& warnings,
                                     int n = 1, bool overlap = false)
{
    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, n, overlap);
    return print.validate(&warnings);
}

size_t count_opt_key(const std::vector<StringObjectException>& warnings, const std::string& key)
{
    return std::count_if(warnings.begin(), warnings.end(),
        [&](const StringObjectException& w) { return w.opt_key == key; });
}

// Make `default_acceleration` exceed the machine's extruding-acceleration limit.
void trigger_acceleration_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{ 100. });
    c.set_key_value("default_acceleration", new ConfigOptionFloatsNullable{ 100000. });
}

// Make `default_jerk` exceed the machine's jerk limit (junction deviation off so
// the jerk check is not skipped).
void trigger_jerk_warning(DynamicPrintConfig& c)
{
    c.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{ 0. });
    c.set_key_value("machine_max_jerk_x", new ConfigOptionFloats{ 1. });
    c.set_key_value("machine_max_jerk_y", new ConfigOptionFloats{ 1. });
    c.set_key_value("default_jerk", new ConfigOptionFloatsNullable{ 9999. });
}

// Precise outer wall is ignored unless the wall sequence is inner-outer.
void trigger_precise_wall_warning(DynamicPrintConfig& c)
{
    c.set_key_value("precise_outer_wall", new ConfigOptionBool(true));
    c.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::OuterInner));
}

} // namespace

// ---------------------------------------------------------------------------
// {first_object_name} filename placeholder
// ---------------------------------------------------------------------------
namespace {

// Add a printable 20mm cube named `name` to `model`; returns it so the caller can tweak it.
ModelObject* add_named_cube(Model& model, const std::string& name)
{
    ModelObject* obj = model.add_object();
    obj->name = name;
    obj->add_volume(make_cube(20.0, 20.0, 20.0));
    obj->add_instance();
    obj->ensure_on_bed();
    return obj;
}

// Resolve `format` to an output file name for a print of `model`. `filename_base`, when set,
// is the saved-project name passed to output_filename().
std::string resolved_output_name(Model& model, const std::string& format, const std::string& filename_base = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filename_format", new ConfigOptionString(format));

    Print print;
    for (ModelObject* obj : model.objects)
        print.auto_assign_extruders(obj);
    print.apply(model, config);
    return print.output_filename(filename_base);
}

} // namespace

TEST_CASE("Print: {first_object_name} names the first printable object on the plate", "[Print]")
{
    Model model;

    SECTION("uses the object's name") {
        add_named_cube(model, "WidgetPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "WidgetPart.gcode");
    }

    SECTION("picks the first when several objects are printable") {
        add_named_cube(model, "FirstPart");
        add_named_cube(model, "SecondPart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "FirstPart.gcode");
    }

    SECTION("skips objects outside the print volume (e.g. on another plate)") {
        // First in model order, but not on the current plate, so is_printable() is false.
        add_named_cube(model, "OtherPlatePart")->instances.front()->print_volume_state = ModelInstancePVS_Fully_Outside;
        add_named_cube(model, "OnPlatePart");
        CHECK(resolved_output_name(model, "{first_object_name}") == "OnPlatePart.gcode");
    }

    SECTION("is empty when the object has no name") {
        add_named_cube(model, "");
        CHECK(resolved_output_name(model, "part_{first_object_name}") == "part_.gcode");
    }
}

TEST_CASE("Print: {first_object_name} is not replaced by the saved-project file name", "[Print]")
{
    // Passing a saved-project file name as the filename_base must not change {first_object_name}.
    Model model;
    add_named_cube(model, "WidgetPart");
    CHECK(resolved_output_name(model, "{first_object_name}", "SavedProject") == "WidgetPart.gcode");
}

TEST_CASE("Print::validate stacks independent warnings", "[Print][validate]")
{
    // Two unrelated checks (region precise-wall + machine acceleration) must each
    // contribute their own entry.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(warnings.size() >= 2);
    CHECK(count_opt_key(warnings, "precise_outer_wall") == 1);  // jump-to key is preserved
    for (const auto& w : warnings)
        CHECK(w.is_warning);                                   // every collected entry is a warning
}

TEST_CASE("Print::validate coalesces motion-ability warnings into one", "[Print][validate]")
{
    // The jerk/junction/acceleration checks are mutually exclusive (gated on a shared
    // key), so adding a second motion trigger must NOT add a second warning.
    DynamicPrintConfig accel_only = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_only);
    std::vector<StringObjectException> w_accel;
    CHECK(validate_cubes(accel_only, w_accel).string.empty());

    DynamicPrintConfig accel_and_jerk = DynamicPrintConfig::full_print_config();
    trigger_acceleration_warning(accel_and_jerk);
    trigger_jerk_warning(accel_and_jerk);
    std::vector<StringObjectException> w_both;
    CHECK(validate_cubes(accel_and_jerk, w_both).string.empty());

    CHECK(w_accel.size() >= 1);
    CHECK(w_both.size() == w_accel.size());  // the extra motion trigger collapses into the same warning
}

TEST_CASE("Print::validate reports the clumping-detection warning", "[Print][validate]")
{
    // A distinct single-shot path: clumping/wrapping detection without a prime tower warns
    // (and carries the enable_prime_tower jump-to key). enable_prime_tower must be off, as
    // the warning lives in the no-prime-tower branch.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "enable_prime_tower") == 1);
}

TEST_CASE("Print::validate concatenates layered-clearance collisions into one warning", "[Print][validate]")
{
    // In by-layer mode, layered_print_cleareance_valid folds every too-close pair into a
    // single warning entry (newline-joined), unlike the per-check stacking above. Isolate
    // that entry by type so unrelated default-config warnings don't affect the assertion.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    std::vector<StringObjectException> warnings;
    StringObjectException err = validate_cubes(config, warnings, /*n=*/3, /*overlap=*/true);

    CHECK(err.string.empty());
    auto is_layered = [](const StringObjectException& w) {
        return w.type == STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT; };
    REQUIRE(std::count_if(warnings.begin(), warnings.end(), is_layered) == 1);  // 3 objects, 2 collisions, 1 entry
    auto it = std::find_if(warnings.begin(), warnings.end(), is_layered);
    CHECK(it->string.find('\n') != std::string::npos);  // the collisions were concatenated
}

TEST_CASE("Print::validate tolerates a null warnings pointer", "[Print][validate]")
{
    // Callers may pass no warnings sink: a warning-producing config must not crash
    // and must still return without a blocking error.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    trigger_precise_wall_warning(config);
    trigger_acceleration_warning(config);

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, /*n=*/1, /*overlap=*/false);

    StringObjectException err = print.validate();  // warnings == nullptr
    CHECK(err.string.empty());
}

TEST_CASE("A default slice emits perimeter, infill, and skirt", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 } // keep recorded Z at the printed height
    });
    CHECK(role_passes(gcode, "perimeter") > 0);
    CHECK(role_passes(gcode, "infill")    > 0);
    CHECK(role_passes(gcode, "skirt")     > 0);
    CHECK_THAT(max_z(gcode), Catch::Matchers::WithinAbs(20.0, 1e-4));
}

// The G-code carries a config-comment block describing the resolved settings. The
// per-region width lines are always present; the support and first-layer lines appear
// only when those features are configured.
TEST_CASE("G-code lists the resolved extrusion-width settings", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, { { "initial_layer_line_width", 0 } });
    CHECK(gcode.find("; external perimeters extrusion width") != std::string::npos);
    CHECK(gcode.find("; perimeters extrusion width")          != std::string::npos);
    CHECK(gcode.find("; infill extrusion width")              != std::string::npos);
    CHECK(gcode.find("; solid infill extrusion width")        != std::string::npos);
    CHECK(gcode.find("; top infill extrusion width")          != std::string::npos);
    CHECK(gcode.find("; support material extrusion width")    == std::string::npos);
    CHECK(gcode.find("; first layer extrusion width")         == std::string::npos);
    CHECK(gcode.find("; layer_height")                        != std::string::npos);
    CHECK(gcode.find("; sparse_infill_density")               != std::string::npos);

    const std::string with_support = slice({ cube(20) }, {
        { "initial_layer_line_width", 0 }, { "enable_support", true }, { "raft_layers", 3 },
    });
    CHECK(with_support.find("; support material extrusion width") != std::string::npos);

    const std::string with_first_layer = slice({ cube(20) }, { { "initial_layer_line_width", "0.5" } });
    CHECK(with_first_layer.find("; first layer extrusion width") != std::string::npos);
}

// gcode_skip_config_block suppresses the resolved-settings block while leaving the
// header and executable blocks intact.
TEST_CASE("gcode_skip_config_block omits the resolved-settings comment block", "[Print]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "gcode_skip_config_block", true },
        { "gcode_comments",         true },
    });
    CHECK(gcode.find("; CONFIG_BLOCK_START")     == std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END")       == std::string::npos);
    CHECK(gcode.find("; layer_height =")         == std::string::npos);
    CHECK(gcode.find("; fill_density =")         == std::string::npos);
    CHECK(gcode.find("; HEADER_BLOCK_START")     != std::string::npos);
    CHECK(gcode.find("; EXECUTABLE_BLOCK_START") != std::string::npos);
}

// Custom G-code templates substitute placeholders during export.
TEST_CASE("Custom G-code placeholders are substituted", "[Print]")
{
    // [current_extruder] in the start G-code.
    CHECK(slice({ cube(20) }, { { "machine_start_gcode", "; Extruder [current_extruder]" } })
              .find("; Extruder 0") != std::string::npos);

    // [layer_num] / [layer_z] in the end G-code (a 20mm cube at 0.1mm is 200 layers).
    const std::string end_gcode = slice({ cube(20) }, {
        { "machine_end_gcode",          "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(end_gcode.find("; Layer_num 199") != std::string::npos);
    CHECK(end_gcode.find("; Layer_z 20")    != std::string::npos);

    // printing_by_object_gcode is emitted between sequentially printed objects.
    CHECK(slice_two_cubes_arranged({
                    { "print_sequence",           "by object" },
                    { "printing_by_object_gcode", "; between-object-gcode" },
                })
              .find("; between-object-gcode") != std::string::npos);

    // [layer_num] keeps counting across sequentially printed objects (199 then 399).
    const std::string per_layer = slice_two_cubes_arranged({
        { "print_sequence",             "by object" },
        { "layer_change_gcode",         ";Layer:[layer_num] ([layer_z] mm)" },
        { "layer_height",               0.1 },
        { "initial_layer_print_height", 0.1 },
    });
    CHECK(per_layer.find(";Layer:199 ") != std::string::npos);
    CHECK(per_layer.find(";Layer:399 ") != std::string::npos);
}

TEST_CASE("export_gcode writes G-code without a result pointer", "[Print][export_gcode]")
{
    Print print;
    Model model;
    Slic3r::Test::init_print({cube(20)}, print, model);
    print.process();

    SECTION("non-BBL printer") {}
    SECTION("BBL printer") { print.is_BBL_printer() = true; }

    ScopedTemporaryFile temp(".gcode");
    REQUIRE_NOTHROW(print.export_gcode(temp.string(), nullptr, nullptr));

    std::ifstream in(temp.string());
    const std::string gcode((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    REQUIRE_FALSE(gcode.empty());
}

TEST_CASE("Sequential printing follows model order", "[Print]")
{
    // Two objects of different heights, taller one added first. Orca prints
    // sequential objects in model order, so the taller one is printed first.
    const std::string gcode = Slic3r::Test::slice({ cube(20), Slic3r::make_cube(20, 20, 10) }, {
        { "print_sequence",             "by object" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "z_hop",                      0 }
    });

    // The first object's height is the peak Z reached before Z drops back to the
    // first layer (the object change). With by-object printing only an object
    // change returns Z to the bottom.
    double first_object_peak_z = 0.0;
    double running_peak        = 0.0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (first_object_peak_z != 0.0 || !line.extruding(self)) return; // ignore travels (e.g. start-gcode Z lift)
        if (running_peak > 1.0 && self.z() < 1.0)
            first_object_peak_z = running_peak;
        else
            running_peak = std::max(running_peak, static_cast<double>(self.z()));
    });

    REQUIRE_THAT(first_object_peak_z, Catch::Matchers::WithinAbs(20.0, 0.3));
}

// A sequential (by-object) print must publish the print-level nozzle group result just
// like a by-layer print, so custom g-code can index the per-nozzle placeholder tables
// (e.g. nozzle_diameter_at_nozzle_id[]) instead of failing on an empty vector.
TEST_CASE("Sequential printing publishes the nozzle group result", "[Print][MultiNozzle]")
{
    SECTION("process() publishes the result") {
        Print print;
        Model model;
        place_two_cubes_apart(60.0, { { "print_sequence", "by object" } }, print, model);
        print.process();
        REQUIRE(print.get_layered_nozzle_group_result() != nullptr);
    }

    SECTION("start g-code can index the per-nozzle diameter table") {
        const std::string gcode = slice_two_cubes_arranged({
            { "print_sequence",      "by object" },
            { "machine_start_gcode", "{if nozzle_diameter_at_nozzle_id[0] > 0}; SEQ-ND-OK\n{endif}" },
        });
        CHECK(gcode.find("; SEQ-ND-OK") != std::string::npos);
    }
}

TEST_CASE("Print::validate records LDM Vase Plus startup and motion warnings", "[Print][validate][ClayVasePlus]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("ldm_modded_printer", new ConfigOptionBool(true));
    config.set_key_value("ldm_disable_retracts", new ConfigOptionBool(true));
    config.set_key_value("ldm_disable_z_hop", new ConfigOptionBool(true));
    config.set_key_value("retraction_length", new ConfigOptionFloats{ 1.5 });
    config.set_key_value("z_hop", new ConfigOptionFloats{ 0.8 });
    config.set_key_value("machine_start_gcode", new ConfigOptionString("G28\nG1 E-1.25 F300\nG1 X97.123 Y4.5 E6.75 F812\n"));

    Slic3r::Model model;
    Slic3r::Print print;
    build_cubes(model, print, config, /*n=*/1, /*overlap=*/false);

    std::vector<StringObjectException> warnings;
    StringObjectException err = print.validate(&warnings);

    CHECK(err.string.empty());
    CHECK(count_opt_key(warnings, "spiral_mode") == 1);
    CHECK(count_opt_key(warnings, "retraction_length") == 1);
    CHECK(count_opt_key(warnings, "z_hop") == 1);
    CHECK(count_opt_key(warnings, "machine_start_gcode") == 2);

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    CHECK(analysis.overall_risk_level == "high_risk");
    CHECK(analysis.startup_compatibility.startup_retract_risk);
    CHECK(analysis.startup_compatibility.purge_like_start_detected);
    CHECK(analysis.metric_snapshot.max_retraction_length_mm == Catch::Approx(1.5));
    CHECK(analysis.metric_snapshot.max_z_hop_mm == Catch::Approx(0.8));
    CHECK(analysis.warnings.size() >= 4);
}

TEST_CASE("LDM Vase Plus body continuity classifies a plain cube as clean control", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    CHECK(analysis.risk_distribution_mode == "clean_control");
    CHECK_FALSE(analysis.body_fragmentation_zone.detected);
}

TEST_CASE("LDM Vase Plus body continuity does not false-flag a gapless cube", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // A plain cube produces no gap fill at all; with the clay bead declared
    // the narrow-gap metric must stay quiet, and config-level retract
    // warnings from validate() must not leak into the geometric
    // classification. (The positive path for base rescue is covered by the
    // julia_mop_clay case in the CI trust gate, which slices real geometry.)
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 }
    });
    std::vector<StringObjectException> warnings;
    print.validate(&warnings); // sets config-level base_rescue state
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    CHECK_FALSE(analysis.base_rescue_complexity.has_gap_infill);
    CHECK(analysis.base_rescue_complexity.level == "none");
    CHECK(analysis.risk_distribution_mode == "clean_control");
}

TEST_CASE("LDM Vase Plus body continuity detects a fragmentation zone on a sphere", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({TestMesh::sphere_50mm}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    // The lower hemisphere produces sustained overhang-wall paths.
    CHECK(analysis.body_fragmentation_zone.detected);
    CHECK(analysis.body_fragmentation_zone.peak_overhang_wall_sections >= 1);
    const bool body_mode = analysis.risk_distribution_mode == "body_spread" || analysis.risk_distribution_mode == "mixed";
    CHECK(body_mode);
    CHECK(analysis.overall_risk_level == "high_risk");
}

TEST_CASE("LDM Vase Plus support margin: vertical walls are safe", "[Print][ClayVasePlus][SupportMargin]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    REQUIRE_FALSE(analysis.support_margin_field.empty());
    CHECK(analysis.support_margin_summary.status == "safe");
    CHECK(analysis.support_margin_summary.worst_margin_mm > 0.0);
}

TEST_CASE("LDM Vase Plus support margin: a 45 degree wall fails the 40 degree envelope", "[Print][ClayVasePlus][SupportMargin]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({TestMesh::slopy_cube}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    REQUIRE_FALSE(analysis.support_margin_field.empty());
    CHECK(analysis.support_margin_summary.status == "failing");
    CHECK(analysis.support_margin_summary.worst_margin_mm < 0.0);
    CHECK(analysis.support_margin_summary.first_warning_z_mm > 0.0);
}

TEST_CASE("LDM Vase Plus support margin: explicit step override wins", "[Print][ClayVasePlus][SupportMargin]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // 5 mm admissible step makes the 45 degree chamfer trivially safe.
    Slic3r::Test::init_print({TestMesh::slopy_cube}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_max_unsupported_step_mm", 5.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    REQUIRE_FALSE(analysis.support_margin_field.empty());
    CHECK(analysis.support_margin_summary.status == "safe");
}

TEST_CASE("LDM reservoir check warns with a run-dry height when capacity is exceeded", "[Print][ClayVasePlus][Reservoir]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // A 20mm cube extrudes far more than 1 ml; the check must fire and
    // report the height where the reservoir runs dry.
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_reservoir_volume_ml", 1.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto refill = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_RESERVOIR_REFILL"; });
    REQUIRE(refill != analysis.warnings.end());
    CHECK(refill->z_hint_mm > 0.0);
    CHECK(refill->z_hint_mm <= 20.0);
}

static bool has_warning(const Slic3r::ClayVasePlusAnalysisResult &a, const char *code)
{
    return std::any_of(a.warnings.begin(), a.warnings.end(),
        [code](const auto &w) { return w.code == code; });
}

TEST_CASE("LDM section-change: a straight-walled cube stays quiet (rule 10)", "[Print][ClayVasePlus][SectionChange]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    // Constant cross-section -> no abrupt section change, and no overhang -> no
    // material-sensitivity flag. This is the standing tumbler-style control.
    CHECK_FALSE(analysis.section_change.detected);
    CHECK_FALSE(analysis.material_sensitive_geometry);
    CHECK_FALSE(has_warning(analysis, "LDM_SECTION_CHANGE_ABRUPT"));
    CHECK_FALSE(has_warning(analysis, "LDM_MATERIAL_SENSITIVE"));
}

TEST_CASE("LDM section-change: a sphere's rapidly changing section is flagged (rule 10)", "[Print][ClayVasePlus][SectionChange]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({TestMesh::sphere_50mm}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    // Near the poles the wall's enclosed area changes sharply between adjacent
    // layers, exceeding the default 0.30 section-change ratio.
    CHECK(analysis.section_change.detected);
    CHECK(analysis.section_change.peak_ratio > 0.30);
    CHECK(has_warning(analysis, "LDM_SECTION_CHANGE_ABRUPT"));
}

TEST_CASE("LDM material-sensitivity: an overhang with no material props is flagged (rule 4)", "[Print][ClayVasePlus][Plasticity]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // 45-degree wall = sustained overhang; with no ldm_wet_yield_strength the
    // B4 stability screen cannot evaluate, so the plasticity note must fill in.
    Slic3r::Test::init_print({TestMesh::slopy_cube}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    REQUIRE_FALSE(analysis.stability.evaluated);
    CHECK(analysis.material_sensitive_geometry);
    CHECK(has_warning(analysis, "LDM_MATERIAL_SENSITIVE"));
}

TEST_CASE("LDM material-sensitivity: quiet once material props are declared (rule 4)", "[Print][ClayVasePlus][Plasticity]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // With material properties set, the B4 stability screen owns the material
    // judgement and the info-level plasticity fallback must step aside.
    Slic3r::Test::init_print({TestMesh::slopy_cube}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 },
        { "filament_density", "1.9" },
        { "ldm_wet_yield_strength", "5.0" },
        { "ldm_e_modulus", "300.0" }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.stability.evaluated);
    CHECK_FALSE(has_warning(analysis, "LDM_MATERIAL_SENSITIVE"));
}

// ---- Edge cases / robustness for the analysis engine ----

TEST_CASE("LDM section-change: ratio 0 disables the check even on a sphere", "[Print][ClayVasePlus][SectionChange]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // The sphere trips section-change at the default 0.30; 0 must disable it.
    Slic3r::Test::init_print({TestMesh::sphere_50mm}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_max_section_change_ratio", 0.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK_FALSE(analysis.section_change.detected);
    CHECK_FALSE(has_warning(analysis, "LDM_SECTION_CHANGE_ABRUPT"));
}

TEST_CASE("LDM section-change: a very high ratio is not tripped by a sphere", "[Print][ClayVasePlus][SectionChange]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // No physical per-layer section change reaches 500%; the upper gate holds.
    Slic3r::Test::init_print({TestMesh::sphere_50mm}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_max_section_change_ratio", 5.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK_FALSE(analysis.section_change.detected);
}

TEST_CASE("LDM analysis: object with no body layers does not crash and reads clean", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // bottom_shell_layers larger than the layer count makes every layer a base
    // layer, so the body-region loops iterate zero times - an edge that must be
    // handled gracefully (no crash, no false body-risk).
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "bottom_shell_layers", 150 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    CHECK(analysis.risk_distribution_mode == "clean_control");
    CHECK_FALSE(analysis.body_fragmentation_zone.detected);
    CHECK_FALSE(analysis.section_change.detected);
}

TEST_CASE("LDM analysis: a multi-object plate is analyzed without crashing", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // The pass analyzes the first object that has layers; two plain cubes must
    // classify clean_control and not fault on the multi-object plate.
    Slic3r::Test::init_print({cube(20), cube(20)}, print, model, {
        { "ldm_modded_printer", true }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK(analysis.clay_mode_active);
    CHECK(analysis.risk_distribution_mode == "clean_control");
}

TEST_CASE("LDM bead compression check flags a high ratio when the nominal bead is absurdly narrow", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // ldm_nominal_bead_width_mm is informational only (does not change the
    // real slicer extrusion width - see its tooltip), so drive the ratio to
    // an extreme via the bead width alone rather than layer_height: an
    // explicit layer_height incompatible with the default nozzle/extrusion
    // width previously made Flow::spacing() go negative and throw. A bead
    // this narrow guarantees mean_layer_height / bead_width > 0.42 no
    // matter what the default layer height actually is.
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 0.02 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto warn = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_BEAD_COMPRESSION_LOW"; });
    REQUIRE(warn != analysis.warnings.end());
    CHECK(warn->severity == "medium");
}

TEST_CASE("LDM bead compression check flags a low ratio when the nominal bead is absurdly wide", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // Mirror of the above: a bead this wide guarantees
    // mean_layer_height / bead_width < 0.15 regardless of the default
    // layer height, without touching layer_height itself.
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 50.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto warn = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_BEAD_COMPRESSION_HIGH"; });
    REQUIRE(warn != analysis.warnings.end());
    CHECK(warn->severity == "medium");
}

TEST_CASE("LDM reservoir check uses the declared current fill instead of assuming full", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // A 1000 mL reservoir never runs dry on a 20mm cube, but a declared
    // 1 mL remaining in the current load must trigger the refill warning
    // (Track B5 stopgap for the Klipper LDM_RESERVOIR_STATUS macro).
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_reservoir_volume_ml", 1000.0 },
        { "ldm_reservoir_current_ml", 1.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto refill = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_RESERVOIR_REFILL"; });
    REQUIRE(refill != analysis.warnings.end());
    CHECK(refill->z_hint_mm > 0.0);
    CHECK(refill->z_hint_mm <= 20.0);
    CHECK_THAT(refill->metric, Catch::Matchers::ContainsSubstring("current_ml=1"));
}

TEST_CASE("LDM reservoir check still assumes a full load when no current fill is declared", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_reservoir_volume_ml", 1000.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto refill = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_RESERVOIR_REFILL"; });
    CHECK(refill == analysis.warnings.end());
}

TEST_CASE("LDM turn-radius check stays off without a configured minimum radius", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // ldm_min_turn_radius_mm left at its default (0 = disabled).
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto warn = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_TURN_RADIUS_TIGHT"; });
    CHECK(warn == analysis.warnings.end());
}

TEST_CASE("LDM turn-radius check flags a tight turn for an absurdly large minimum radius", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // 1000mm is far beyond any turn radius a 20mm cube can offer, so this
    // guarantees the check fires regardless of exact corner curvature math.
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 },
        { "ldm_min_turn_radius_mm", 1000.0 }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    const auto warn = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_TURN_RADIUS_TIGHT"; });
    REQUIRE(warn != analysis.warnings.end());
    CHECK(warn->severity == "medium");
}

TEST_CASE("LDM Vase Plus clay-native startup strips purge-like start lines", "[Print][ClayVasePlus]")
{
    // Moved from the now-removed test_printgcode.cpp during the 2026-07-11
    // upstream rebase (test suite reorganization); unchanged otherwise.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "initial_layer_line_width",   0 },
        { "gcode_comments",             true },
        { "ldm_modded_printer",               true },
        { "ldm_start_gcode_mode",      "clay_native" },
        { "machine_start_gcode",        "G28\nG1 E-1.25 F300\nG1 X97.123 Y4.5 E6.75 F812\nM117 clay\n" },
        { "z_hop",                      0 }
    });

    std::string gcode = Slic3r::Test::gcode(print);

    REQUIRE(gcode.find("; LDM Vase Plus removed startup line: G1 E-1.25 F300") != std::string::npos);
    REQUIRE(gcode.find("; LDM Vase Plus removed startup line: G1 X97.123 Y4.5 E6.75 F812") != std::string::npos);
    REQUIRE(gcode.find("\nG1 E-1.25 F300\n") == std::string::npos);
    REQUIRE(gcode.find("\nG1 X97.123 Y4.5 E6.75 F812\n") == std::string::npos);
    REQUIRE(gcode.find("M117 clay") != std::string::npos);
}

TEST_CASE("LDM Vase Plus writes an analysis sidecar next to exported G-code", "[Print][ClayVasePlus]")
{
    // Moved from the now-removed test_printgcode.cpp during the 2026-07-11
    // upstream rebase (test suite reorganization); unchanged otherwise.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer",               true }
    });
    print.set_status_silent();
    print.process();

    boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("clay-sidecar-%%%%%%%%.gcode");
    print.export_gcode(gcode_path.string(), nullptr, nullptr);
    boost::filesystem::path sidecar = gcode_path;
    sidecar += ".ldm-analysis.json";

    REQUIRE(boost::filesystem::exists(sidecar));
    std::ifstream sidecar_stream(sidecar.string());
    nlohmann::json j = nlohmann::json::parse(sidecar_stream);
    CHECK(j["ldm_active"] == true);
    CHECK(j["support_margin_summary"]["status"] == "safe");
    CHECK(j["support_margin_field"].size() > 0);
    // No flow check entered: the fingerprint must exist but say unmeasured.
    CHECK(j["load_fingerprint"]["measured"] == false);

    // Close the read handle before removing: on Windows removing a still-open
    // file is a sharing violation (POSIX allows unlinking an open file, so this
    // only bit the Windows test runs).
    sidecar_stream.close();
    boost::filesystem::remove(gcode_path);
    boost::filesystem::remove(sidecar);
}

TEST_CASE("LDM sidecar tags the print with the measured load fingerprint", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // Values as the machine flow check (LDM_FLOW_TUNE / blob-scale) would
    // report them: g per commanded E-mm and the auger-slip ceiling.
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_flow_multiplier_measured", 0.0378 },
        { "ldm_flow_ceiling_mm_s", 12.0 }
    });
    print.set_status_silent();
    print.process();

    boost::filesystem::path gcode_path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("clay-fingerprint-%%%%%%%%.gcode");
    print.export_gcode(gcode_path.string(), nullptr, nullptr);
    boost::filesystem::path sidecar = gcode_path;
    sidecar += ".ldm-analysis.json";

    REQUIRE(boost::filesystem::exists(sidecar));
    std::ifstream sidecar_stream(sidecar.string());
    nlohmann::json j = nlohmann::json::parse(sidecar_stream);
    CHECK(j["load_fingerprint"]["measured"] == true);
    CHECK_THAT(j["load_fingerprint"]["flow_multiplier_g_per_e"].get<double>(),
               Catch::Matchers::WithinAbs(0.0378, 1e-6));
    CHECK_THAT(j["load_fingerprint"]["flow_ceiling_mm_s"].get<double>(),
               Catch::Matchers::WithinAbs(12.0, 1e-6));

    // Close the read handle before removing (Windows: removing an open file is
    // a sharing violation).
    sidecar_stream.close();
    boost::filesystem::remove(gcode_path);
    boost::filesystem::remove(sidecar);
}

TEST_CASE("LDM stability screening stays off without declared material properties", "[Print][ClayVasePlus][Stability]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 }
        // density / yield / modulus left at defaults (0) -> screening off
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK_FALSE(analysis.stability.evaluated);
    CHECK(analysis.stability.predicted_mode == "stable");
}

TEST_CASE("LDM stability screening flags squash for an absurdly weak material", "[Print][ClayVasePlus][Stability]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    // 5 g/cm3 paste with 0.01 kPa yield: a 20mm cube must exceed the squash
    // limit (sigma = rho*g*h ~ 981 Pa vs 10 Pa capacity).
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "ldm_modded_printer", true },
        { "ldm_nominal_bead_width_mm", 4.62 },
        { "filament_density", "5" },
        { "ldm_wet_yield_strength", "0.01" }
    });
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    REQUIRE(analysis.stability.evaluated);
    CHECK(analysis.stability.squash_ratio > 1.0);
    CHECK(analysis.stability.predicted_mode == "squash");
    const auto warn = std::find_if(analysis.warnings.begin(), analysis.warnings.end(),
        [](const auto &w) { return w.code == "LDM_STABILITY_SQUASH"; });
    REQUIRE(warn != analysis.warnings.end());
    CHECK(warn->severity == "high");
}

TEST_CASE("LDM Vase Plus body continuity stays inert when clay mode is off", "[Print][ClayVasePlus]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({TestMesh::sphere_50mm}, print, model);
    print.process();

    const auto &analysis = print.clay_vase_plus_analysis();
    CHECK_FALSE(analysis.clay_mode_active);
    CHECK_FALSE(analysis.body_fragmentation_zone.detected);
    CHECK(analysis.warnings.empty());
}
