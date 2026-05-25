// Tests for the schema-driven action argument pipeline.
//
// Covers:
//   - Schema lookup (Action::schemaFor) for every built-in action.
//   - Adapter (Action::coercePositional) — positional string args ->
//     typed ArgsValue, including int parsing, direction validation,
//     and required-field enforcement.
//   - Builder (Action::buildActionFromArgs) — typed ArgsValue ->
//     Action::Any variant, with one builder per action.
//   - parseAction / parseActionTyped end-to-end paths.
//   - Aliases (scroll_to_previous_prompt, scroll_to_next_prompt).
//   - Script-action passthrough (name contains ".").
//
// These tests pin the typed-args refactor's contract: any change that
// breaks the positional adapter is a back-compat regression for
// existing TOML configs and JS scripts using the variadic form.

#include "Action.h"
#include "Bindings.h"
#include <doctest/doctest.h>

TEST_CASE("Action schema: every built-in variant has a registered schema" * doctest::test_suite("action_args"))
{
    // Every variant index resolves to a schema (possibly empty for
    // nullary actions). Drift detector: adding a new variant in
    // Action.h without registering a schema in Action.cpp fires this.
    for (Action::TypeIndex i = 0; i < Action::count; ++i) {
        auto name = Action::nameOf(i);
        CAPTURE(name);
        const auto *schema = Action::schemaFor(name);
        REQUIRE(schema != nullptr);
    }
}

TEST_CASE("Action adapter: positional strings -> ArgsValue" * doctest::test_suite("action_args"))
{
    SUBCASE("nullary action accepts no args")
    {
        std::string err;
        const auto *schema = Action::schemaFor("NewTab");
        REQUIRE(schema != nullptr);
        auto args = Action::coercePositional(*schema, {}, err);
        REQUIRE(args.has_value());
        CHECK(args->empty());
    }

    SUBCASE("required direction arg validates value")
    {
        std::string err;
        const auto *schema = Action::schemaFor("SplitPane");
        REQUIRE(schema != nullptr);

        auto good = Action::coercePositional(*schema, { "right" }, err);
        REQUIRE(good.has_value());
        REQUIRE(good->count("direction") == 1);
        CHECK(good->at("direction").get<std::string>() == "right");

        // Unknown value rejected.
        auto bad = Action::coercePositional(*schema, { "diagonal" }, err);
        CHECK(!bad.has_value());
        CHECK(!err.empty());
    }

    SUBCASE("missing required arg rejected with error")
    {
        std::string err;
        const auto *schema = Action::schemaFor("SplitPane");
        REQUIRE(schema != nullptr);
        auto missing = Action::coercePositional(*schema, {}, err);
        CHECK(!missing.has_value());
        CHECK(err.find("direction") != std::string::npos);
    }

    SUBCASE("optional arg falls back to default")
    {
        std::string err;
        const auto *schema = Action::schemaFor("ScrollUp");
        REQUIRE(schema != nullptr);
        // No args -> default 3.
        auto args = Action::coercePositional(*schema, {}, err);
        REQUIRE(args.has_value());
        REQUIRE(args->count("lines") == 1);
        CHECK(args->at("lines").get<int64_t>() == 3);

        // Explicit override.
        auto override_ = Action::coercePositional(*schema, { "7" }, err);
        REQUIRE(override_.has_value());
        CHECK(override_->at("lines").get<int64_t>() == 7);
    }

    SUBCASE("int parser rejects non-numeric")
    {
        std::string err;
        const auto *schema = Action::schemaFor("ScrollUp");
        REQUIRE(schema != nullptr);
        auto bad = Action::coercePositional(*schema, { "abc" }, err);
        CHECK(!bad.has_value());
    }

    SUBCASE("trailing args past the schema are ignored")
    {
        std::string err;
        const auto *schema = Action::schemaFor("SplitPane");
        REQUIRE(schema != nullptr);
        auto args = Action::coercePositional(*schema, { "right", "stray" }, err);
        REQUIRE(args.has_value());
        CHECK(args->size() == 1);
    }

    SUBCASE("AdjustPaneSize: direction required + optional amount")
    {
        std::string err;
        const auto *schema = Action::schemaFor("AdjustPaneSize");
        REQUIRE(schema != nullptr);

        auto minimal = Action::coercePositional(*schema, { "left" }, err);
        REQUIRE(minimal.has_value());
        CHECK(minimal->at("direction").get<std::string>() == "left");
        CHECK(minimal->at("amount").get<int64_t>() == 1);

        auto explicit_ = Action::coercePositional(*schema, { "left", "5" }, err);
        REQUIRE(explicit_.has_value());
        CHECK(explicit_->at("amount").get<int64_t>() == 5);
    }
}

TEST_CASE("Action builder: ArgsValue -> Action::Any variant" * doctest::test_suite("action_args"))
{
    SUBCASE("NewTab from empty args")
    {
        std::string err;
        auto built = Action::buildActionFromArgs("NewTab", {}, err);
        REQUIRE(built.has_value());
        CHECK(std::holds_alternative<Action::NewTab>(*built));
    }

    SUBCASE("SplitPane direction wired into variant field")
    {
        std::string err;
        Action::ArgsValue args;
        args["direction"] = Action::ArgValue { std::string { "down" } };
        auto built        = Action::buildActionFromArgs("SplitPane", args, err);
        REQUIRE(built.has_value());
        REQUIRE(std::holds_alternative<Action::SplitPane>(*built));
        CHECK(std::get<Action::SplitPane>(*built).dir == Action::Direction::Down);
    }

    SUBCASE("AdjustPaneSize int coercion through builder")
    {
        std::string err;
        Action::ArgsValue args;
        args["direction"] = Action::ArgValue { std::string { "right" } };
        args["amount"]    = Action::ArgValue { int64_t { 4 } };
        auto built        = Action::buildActionFromArgs("AdjustPaneSize", args, err);
        REQUIRE(built.has_value());
        const auto &a = std::get<Action::AdjustPaneSize>(*built);
        CHECK(a.dir == Action::Direction::Right);
        CHECK(a.amount == 4);
    }

    SUBCASE("Unknown action name rejected")
    {
        std::string err;
        auto built = Action::buildActionFromArgs("NotAThing", {}, err);
        CHECK(!built.has_value());
        CHECK(!err.empty());
    }
}

TEST_CASE("parseAction (positional, schema-driven)" * doctest::test_suite("action_args"))
{
    SUBCASE("snake_case names translate to Pascal")
    {
        auto a = parseAction("new_tab", {});
        REQUIRE(a.has_value());
        CHECK(std::holds_alternative<Action::NewTab>(*a));
    }

    SUBCASE("aliases route through baked positional args")
    {
        auto prev = parseAction("scroll_to_previous_prompt", {});
        REQUIRE(prev.has_value());
        REQUIRE(std::holds_alternative<Action::ScrollToPrompt>(*prev));
        CHECK(std::get<Action::ScrollToPrompt>(*prev).direction == -1);

        auto next = parseAction("scroll_to_next_prompt", {});
        REQUIRE(next.has_value());
        CHECK(std::get<Action::ScrollToPrompt>(*next).direction == 1);
    }

    SUBCASE("script actions pass through verbatim")
    {
        auto a = parseAction("myns.foo", { "arg1", "arg2" });
        REQUIRE(a.has_value());
        REQUIRE(std::holds_alternative<Action::ScriptAction>(*a));
        const auto &sa = std::get<Action::ScriptAction>(*a);
        CHECK(sa.name == "myns.foo");
        REQUIRE(sa.args.size() == 2);
        CHECK(sa.args[0] == "arg1");
        CHECK(sa.args[1] == "arg2");
    }
}

TEST_CASE("parseActionTyped (named, schema-driven)" * doctest::test_suite("action_args"))
{
    SUBCASE("typed direction arg builds the variant")
    {
        Action::ArgsValue args;
        args["direction"] = Action::ArgValue { std::string { "up" } };
        auto a            = parseActionTyped("split_pane", args);
        REQUIRE(a.has_value());
        CHECK(std::get<Action::SplitPane>(*a).dir == Action::Direction::Up);
    }

    SUBCASE("script actions: object args flatten into positional ScriptAction args")
    {
        Action::ArgsValue args;
        args["name"] = Action::ArgValue { std::string { "claude" } };
        auto a       = parseActionTyped("pane.activate_by_name", args);
        REQUIRE(a.has_value());
        REQUIRE(std::holds_alternative<Action::ScriptAction>(*a));
        const auto &sa = std::get<Action::ScriptAction>(*a);
        CHECK(sa.name == "pane.activate_by_name");
        // One positional carrying the name value (any string in args
        // gets flattened; numeric values get to_string'd).
        REQUIRE(sa.args.size() == 1);
        CHECK(sa.args[0] == "claude");
    }
}
