// The constants of the data model (limits.hpp) and the fault-injection hooks (testing.hpp).
#include <catch2/catch.hpp>

#include "format/limits.hpp"
#include "format/testing.hpp"

using namespace loomora::format;

TEST_CASE("the defaults are the values of the data-model constants table", "[format][limits]") {
    ScopedLimits scoped;
    reset_limits();
    const Limits &l = limits();
    REQUIRE(l.warn_nails_per_layout == 100000u);
    REQUIRE(l.warn_steps_per_result == 5000000u);
    REQUIRE(l.warn_triangles_per_mesh == 5000000u);
    REQUIRE(l.warn_decompressed_total == 256u * 1024u * 1024u);
    REQUIRE(l.warn_items_per_kind == 1000u);
    REQUIRE(l.bomb_ratio == 250u);
    REQUIRE(l.bomb_min_bytes == 1024u * 1024u);
    REQUIRE(l.thumb_max_side == 256u);
    REQUIRE(l.thumb_max_bytes == 64u * 1024u);
    REQUIRE(l.nail_in_frame_tolerance_mm == 0.01);
    REQUIRE(l.nail_overlap_tolerance_mm == 1e-6);
    REQUIRE(l.frame_origin_tolerance_mm == 0.01);
    REQUIRE(l.json_depth == 64u);
    REQUIRE(l.json_entry_max_bytes == 1024ull * 1024 * 1024);
    REQUIRE(l.entry_name_max_bytes == 512u);
    REQUIRE(l.progress_interval_bytes == 4u * 1024u * 1024u);
    REQUIRE(l.zip64_size_threshold == 0xFFFFFFFFull);
    REQUIRE(l.zip64_offset_threshold == 0xFFFFFFFFull);
    REQUIRE(l.zip64_count_threshold == 0xFFFFu);
}

TEST_CASE("a limit can be lowered and ScopedLimits puts the defaults back", "[format][limits]") {
    {
        ScopedLimits scoped;
        limits().warn_nails_per_layout = 3;
        limits().bomb_ratio = 2;
        REQUIRE(limits().warn_nails_per_layout == 3u);
    }
    REQUIRE(limits().warn_nails_per_layout == 100000u);
    REQUIRE(limits().bomb_ratio == 250u);
}

TEST_CASE("set_limit_by_name knows the names of the CLI option", "[format][limits]") {
    ScopedLimits scoped;
    REQUIRE(set_limit_by_name("warn_nails", 5));
    REQUIRE(set_limit_by_name("warn_steps", 6));
    REQUIRE(set_limit_by_name("warn_triangles", 7));
    REQUIRE(set_limit_by_name("warn_total_bytes", 8));
    REQUIRE(set_limit_by_name("warn_items", 9));
    REQUIRE(set_limit_by_name("bomb_ratio", 10));
    REQUIRE(limits().warn_nails_per_layout == 5u);
    REQUIRE(limits().warn_steps_per_result == 6u);
    REQUIRE(limits().warn_triangles_per_mesh == 7u);
    REQUIRE(limits().warn_decompressed_total == 8u);
    REQUIRE(limits().warn_items_per_kind == 9u);
    REQUIRE(limits().bomb_ratio == 10u);
    REQUIRE_FALSE(set_limit_by_name("nope", 1));
    REQUIRE_FALSE(set_limit_by_name(nullptr, 1));
}

TEST_CASE("the fault hooks are null in production and cleared by ScopedHooks", "[format][testing]") {
    REQUIRE_FALSE(should_fail_write(0, 1));
    REQUIRE_FALSE(should_fail_flush());
    REQUIRE_FALSE(should_fail_rename());
    REQUIRE_FALSE(should_fail_alloc(16));
    {
        ScopedHooks scoped;
        hooks().fail_flush = [](void *) { return true; };
        hooks().fail_rename = [](void *) { return true; };
        REQUIRE(should_fail_flush());
        REQUIRE(should_fail_rename());
        REQUIRE_FALSE(should_fail_write(0, 1));
    }
    REQUIRE_FALSE(should_fail_flush());
    REQUIRE_FALSE(should_fail_rename());
}
