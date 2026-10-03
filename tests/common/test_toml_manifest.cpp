#include <catch_amalgamated.hpp>

#include <string>

#include "pspocket/recomp/manifest.hpp"
#include "pspocket/recomp/toml.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

namespace {

const toml::Value& at(const toml::Value& t, std::string_view key)
{
    const toml::Value* v = t.get(key);
    REQUIRE(v != nullptr);
    return *v;
}

std::string toml_error(std::string_view text)
{
    const auto r = toml::parse(text);
    REQUIRE_FALSE(r);
    return r.error().message;
}

constexpr std::string_view kValidManifest = R"(
title_id = "BLUS30127"
title = "Test Title"
app_version = "01.02"
iso_sha256 = "ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789"

[[files]]
path = "dev_hdd0/game/BLUS30127/USRDIR/EBOOT.elf"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"

[[overrides]]
addr = 0x00010200
name = "game_override"

[[patches]]
addr = 0x00010300
bytes = "60000000 4E800020"
why = "skip a render-target readback"

[[patches]]
addr = 0x00010308
bytes = "38600000"
why = "return 0"

[[spu_programs]]
sha1 = "0123456789ABCDEF0123456789abcdef01234567"
name = "edge_zlib"
action = "replace"

[[spu_programs]]
sha1 = "1111111111111111111111111111111111111111"
name = "custom_job"
action = "lift"
)";

std::string manifest_error(std::string_view text, const ManifestOptions& options = {})
{
    const auto r = parse_manifest(text, options);
    REQUIRE_FALSE(r);
    return r.error().message;
}

std::string replaced(std::string_view from, std::string_view to)
{
    std::string s(kValidManifest);
    const auto pos = s.find(from);
    REQUIRE(pos != std::string::npos);
    s.replace(pos, from.size(), to);
    return s;
}

} // namespace

TEST_CASE("TOML subset: values, tables, arrays of tables")
{
    const auto r = toml::parse(R"(# comment
name = "basic \"string\"\t\u00e9"   # trailing comment
literal = 'C:\no\escapes'
"quoted key" = 1
dotted.key = 2
hex = 0xDEAD_beef
oct = 0o755
bin = 0b1010
neg = -42
plus = +7
big = 9_223_372_036_854_775_807
yes = true
no = false
list = [1, 2,
        3,]   # trailing comma, multi-line
nested = [[1, 2], ["a"]]
empty = []

[table]
x = 1
[table.sub]
y = "z"

[[items]]
id = 1
[[items]]
id = 2
[items.extra]
flag = true
)");
    REQUIRE(r);
    const toml::Value& root = *r;
    CHECK(at(root, "name").string == "basic \"string\"\t\xC3\xA9");
    CHECK(at(root, "literal").string == "C:\\no\\escapes");
    CHECK(at(root, "quoted key").integer == 1);
    CHECK(at(at(root, "dotted"), "key").integer == 2);
    CHECK(at(root, "hex").integer == 0xDEADBEEF);
    CHECK(at(root, "oct").integer == 0755);
    CHECK(at(root, "bin").integer == 10);
    CHECK(at(root, "neg").integer == -42);
    CHECK(at(root, "plus").integer == 7);
    CHECK(at(root, "big").integer == 9223372036854775807);
    CHECK(at(root, "yes").boolean);
    CHECK_FALSE(at(root, "no").boolean);
    REQUIRE(at(root, "list").items.size() == 3);
    CHECK(at(root, "list").items[2].integer == 3);
    CHECK(at(root, "nested").items[1].items[0].string == "a");
    CHECK(at(root, "empty").items.empty());
    CHECK(at(at(root, "table"), "x").integer == 1);
    CHECK(at(at(at(root, "table"), "sub"), "y").string == "z");
    const auto& items = at(root, "items");
    REQUIRE(items.items.size() == 2);
    CHECK(at(items.items[0], "id").integer == 1);
    CHECK(at(at(items.items[1], "extra"), "flag").boolean);
    // Keys keep their definition order.
    CHECK(root.keys.front() == "name");
}

TEST_CASE("TOML subset: errors name the line and reject unsupported syntax")
{
    CHECK(toml_error("a = 1\na = 2\n").find("line 2") != std::string::npos);
    CHECK(toml_error("a = 1\na = 2\n").find("duplicate key 'a'") != std::string::npos);
    CHECK(toml_error("x = 1.5\n").find("floats are not supported") != std::string::npos);
    CHECK(toml_error("x = 1e3\n").find("floats") != std::string::npos);
    CHECK(toml_error("x = inf\n").find("floats") != std::string::npos);
    CHECK(toml_error("x = { a = 1 }\n").find("inline tables") != std::string::npos);
    CHECK(toml_error("x = \"\"\"multi\"\"\"\n").find("multi-line") != std::string::npos);
    CHECK(toml_error("d = 1979-05-27\n").find("dates") != std::string::npos);
    CHECK(toml_error("x = \"open\n").find("newline in a string") != std::string::npos);
    CHECK(toml_error("x = \"bad \\q escape\"\n").find("unknown escape") != std::string::npos);
    CHECK(toml_error("x = 0123\n").find("leading zeros") != std::string::npos);
    CHECK(toml_error("x = 99999999999999999999\n").find("out of range") != std::string::npos);
    CHECK(toml_error("x = -0x10\n").find("sign") != std::string::npos);
    CHECK(toml_error("x = 1__0\n").find("'_'") != std::string::npos);
    CHECK(toml_error("x 1\n").find("expected '='") != std::string::npos);
    CHECK(toml_error("x = 1 y = 2\n").find("end of the line") != std::string::npos);
    CHECK(toml_error("x = [1, 2\n").find("unterminated array") != std::string::npos);
    CHECK(toml_error("[t]\n[t]\n").find("defined twice") != std::string::npos);
    CHECK(toml_error("a = 1\n[a]\n").find("defined twice") != std::string::npos);
    CHECK(toml_error("a = 1\n[[a]]\n").find("not an array of tables") != std::string::npos);
    CHECK(toml_error("a = [1]\n[a.b]\n").find("is an array") != std::string::npos);
    CHECK(toml_error("x = tru\n").find("expected a value") != std::string::npos);
    CHECK(toml_error("x = \"\x01\"\n").find("control character") != std::string::npos);
    CHECK(toml_error(std::string(70, '[') + "\n").size() > 0);
    std::string deep = "x = " + std::string(100, '[') + std::string(100, ']') + "\n";
    CHECK(toml_error(deep).find("nested too deeply") != std::string::npos);

    // A table implied by a dotted header can still be defined explicitly once.
    CHECK(toml::parse("[a.b]\nx = 1\n[a]\ny = 2\n"));
}

TEST_CASE("manifest: a valid document")
{
    const auto r = parse_manifest(kValidManifest);
    REQUIRE(r);
    const GameManifest& m = *r;
    CHECK(m.title_id == "BLUS30127");
    CHECK(m.title == "Test Title");
    CHECK(m.app_version == "01.02");
    CHECK(m.iso_sha256 == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"); // lowercased
    REQUIRE(m.files.size() == 1);
    CHECK(m.files[0].path == "dev_hdd0/game/BLUS30127/USRDIR/EBOOT.elf");
    REQUIRE(m.overrides.size() == 1);
    CHECK(m.overrides[0].addr == 0x10200u);
    CHECK(m.overrides[0].name == "game_override");
    REQUIRE(m.patches.size() == 2);
    CHECK(m.patches[0].addr == 0x10300u);
    CHECK(m.patches[0].bytes == std::vector<u8>{0x60, 0x00, 0x00, 0x00, 0x4E, 0x80, 0x00, 0x20});
    CHECK(m.patches[0].why == "skip a render-target readback");
    REQUIRE(m.spu_programs.size() == 2);
    CHECK(m.spu_programs[0].sha1 == "0123456789abcdef0123456789abcdef01234567");
    CHECK(m.spu_programs[0].action == SpuAction::Replace);
    CHECK(m.spu_programs[1].action == SpuAction::Lift);
    CHECK(validate_manifest(m).empty());
    CHECK(validate_manifest(m, ManifestOptions{true}).empty());
}

TEST_CASE("manifest: validation errors")
{
    CHECK(manifest_error(replaced("why = \"return 0\"", "")).find("patches[1]: missing 'why'") != std::string::npos);
    CHECK(manifest_error(replaced("why = \"return 0\"", "why = \"  \"")).find("'why' must explain") != std::string::npos);
    CHECK(manifest_error(replaced("action = \"lift\"", "action = \"jit\"")).find("action 'jit'") != std::string::npos);
    CHECK(manifest_error(replaced("1111111111111111111111111111111111111111", "1111")).find("sha1 is not 40 hex") != std::string::npos);
    CHECK(manifest_error(replaced("BLUS30127\"", "GTA4\"")).find("title_id 'GTA4'") != std::string::npos);
    CHECK(manifest_error(replaced("\"01.02\"", "\"1.2\"")).find("app_version") != std::string::npos);
    CHECK(manifest_error(replaced("title = ", "titel = \"x\"\ntitle = ")).find("unknown key 'titel'") != std::string::npos);
    CHECK(manifest_error(replaced("addr = 0x00010200", "addr = 0x00010202")).find("not 4-byte aligned") != std::string::npos);
    CHECK(manifest_error(replaced("addr = 0x00010200", "addr = \"0x10200\"")).find("must be an integer") != std::string::npos);
    CHECK(manifest_error(replaced("addr = 0x00010200", "addr = 0x100000000")).find("not a 32-bit guest address") != std::string::npos);
    CHECK(manifest_error(replaced("name = \"game_override\"", "name = \"not an identifier\"")).find("C identifier") != std::string::npos);
    CHECK(manifest_error(replaced("addr = 0x00010308", "addr = 0x00010304")).find("overlaps patches[0]") != std::string::npos);
    CHECK(manifest_error(replaced("\"38600000\"", "\"386\"")).find("not an even number of hex digits") != std::string::npos);
    CHECK(manifest_error(replaced("dev_hdd0/game", "/dev_hdd0/game")).find("must be relative") != std::string::npos);
    CHECK(manifest_error(replaced("dev_hdd0/game", "../game")).find("must not contain '..'") != std::string::npos);
    CHECK(manifest_error(replaced("\"edge_zlib\"", "\"custom_job\"\nsha1x = 1")).find("unknown key 'sha1x'") != std::string::npos);
    CHECK(manifest_error(replaced("[[files]]", "files = 3\n[[filez]]")).find("'files' must be an array of tables") != std::string::npos);
    // Every problem is listed, not just the first.
    const auto many = manifest_error(replaced("title = \"Test Title\"", "titel = \"x\"\napp_version2 = 1"));
    CHECK(many.find("unknown key 'titel'") != std::string::npos);
    CHECK(many.find("unknown key 'app_version2'") != std::string::npos);
    CHECK(many.find("missing 'title'") != std::string::npos);
    // TOML syntax errors surface as manifest errors.
    CHECK(manifest_error("title_id = 1.0\n").find("floats") != std::string::npos);
}

TEST_CASE("manifest: required hashes in build mode")
{
    const std::string draft = R"(
title_id = "BLES00229"
title = "Draft"
app_version = "01.08"
iso_sha256 = ""
[[files]]
path = "dev_hdd0/game/BLES00229/USRDIR/EBOOT.elf"
sha256 = ""
)";
    REQUIRE(parse_manifest(draft));
    const auto err = manifest_error(draft, ManifestOptions{true});
    CHECK(err.find("iso_sha256 is required") != std::string::npos);
    CHECK(err.find("files[0]: sha256 is required") != std::string::npos);
}

TEST_CASE("games/gta4/manifest.toml is a valid draft")
{
    const auto r = load_manifest(std::string(PSPOCKET_SOURCE_DIR) + "/games/gta4/manifest.toml");
    if (!r) FAIL(r.error().message);
    const GameManifest& m = *r;
    CHECK(m.title_id == "BLES00229");
    CHECK(m.app_version == "01.08");
    CHECK(m.iso_sha256.empty());
    CHECK_FALSE(m.files.empty());
    for (const auto& f : m.files) CHECK(f.sha256.empty());
    CHECK(m.patches.empty());
    // Not buildable until Phase 0 fills in the hashes.
    CHECK_FALSE(load_manifest(std::string(PSPOCKET_SOURCE_DIR) + "/games/gta4/manifest.toml", ManifestOptions{true}));
}
