// sceNpTrophy's file formats (synthetic: no game data): the XML subset RPCS3 reads with pugixml, TROPHY.TRP as RPCS3's
// TRPLoader reads and installs it, and TROPUSR.DAT as RPCS3's TROPUSRLoader generates, saves and reads it.

#include <catch_amalgamated.hpp>

#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "trophy_files.hpp" // runtime/hle/src (internal)
#include "trophy_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
namespace fsys = std::filesystem;

namespace {

// A temporary host directory, removed at the end.
struct TempDir {
    fsys::path path;
    explicit TempDir(std::string_view name)
        : path(fsys::temp_directory_path() / std::format("pspocket-hle-trophyfiles-{}-{}", name, ::getpid()))
    {
        fsys::remove_all(path);
        fsys::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fsys::remove_all(path, ec);
    }
};

std::string read_text(const fsys::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const fsys::path& p, std::string_view text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<u8> read_bytes(const fsys::path& p)
{
    const std::string s = read_text(p);
    return std::vector<u8>(s.begin(), s.end());
}

} // namespace

TEST_CASE("trophy files: XML as RPCS3 reads it with pugixml's default options", "[hle][trophy]")
{
    // A byte order mark, a comment before the declaration, a DOCTYPE with an internal subset, whitespace between
    // elements (not kept), entities, character references, CDATA, line ends, whitespace in attribute values.
    const std::string xml = "\xEF\xBB\xBF<!--Sce-Np-Trophy-Signature: 0123-->\r\n"
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                            "<!DOCTYPE trophyconf [ <!ENTITY e \"v>\"> ]>\n"
                            "<trophyconf version='1.0'>\n"
                            "  <title-name>A &amp; B &lt;&#65;&#x42;&gt; &unknown; &quot;q&apos; &amp</title-name>\n"
                            "  <?pi inside?><!-- a comment -->\n"
                            "  <trophy id=\" 7\" a=\"x\ty\r\nz\" a=\"second\"><name><![CDATA[<raw> &amp;]]></name>"
                            "<detail>line1\r\nline2\rline3</detail><empty/><blank>   </blank>"
                            "<mixed>  text <b/>more</mixed></trophy>\n"
                            "</trophyconf>\n";
    const std::optional<trophy::XmlNode> doc = trophy::parse_xml(xml);
    REQUIRE(doc);
    REQUIRE(doc->children.size() == 1);
    const trophy::XmlNode& root = trophy::trophy_root(*doc);
    REQUIRE(&root == &doc->children[0]);
    CHECK(root.name == "trophyconf");
    CHECK(root.attribute("version") == "1.0");
    REQUIRE(root.children.size() == 2);
    CHECK(root.children[0].name == "title-name");
    CHECK(root.children[0].content() == "A & B <AB> &unknown; \"q' &amp");
    const trophy::XmlNode& t = root.children[1];
    CHECK(t.name == "trophy");
    CHECK(trophy::atoi(t.attribute("id")) == 7);
    CHECK(t.attribute("a") == "x y z"); // the first of two; \t and \r\n become spaces
    CHECK(t.attribute("missing").empty());
    REQUIRE(t.children.size() == 5);
    CHECK(t.children[0].content() == "<raw> &amp;");
    CHECK(t.children[1].content() == "line1\nline2\nline3");
    CHECK(t.children[2].name == "empty");
    CHECK(t.children[2].content().empty());
    CHECK(t.children[3].children.empty()); // whitespace only: no text node
    CHECK(t.children[3].content().empty());
    CHECK(t.children[4].content() == "  text "); // the first text child, untrimmed
    CHECK(t.content().empty());

    // Malformed documents fail as pugixml's load_buffer does.
    for (const char* bad : {"<a><b></a>", "<a>", "<a x=1/>", "<a x='1/>", "<!-- only a comment -->", "",
             "<a><!-- unterminated </a>", "<a></b>", "</a>", "<a><![CDATA[x</a>", "<?pi"}) {
        INFO(bad);
        CHECK_FALSE(trophy::parse_xml(bad));
    }
    // Several top-level elements are fine (pugixml), and the root is the first one only if it is <trophyconf>.
    const std::optional<trophy::XmlNode> other = trophy::parse_xml("<other><trophy id='1'/></other><trophyconf/>");
    REQUIRE(other);
    CHECK(other->children.size() == 2);
    CHECK(&trophy::trophy_root(*other) == &*other);

    CHECK(trophy::atoi("-1") == -1);
    CHECK(static_cast<u32>(trophy::atoi("-1")) == 0xFFFFFFFFu);
    CHECK(trophy::atoi("000") == 0);
    CHECK(trophy::atoi("042") == 42);
    CHECK(trophy::atoi(" +12ab") == 12);
    CHECK(trophy::atoi("") == 0);
    CHECK(trophy::atoi("x1") == 0);
}

TEST_CASE("trophy files: TROPHY.TRP read and installed as RPCS3's TRPLoader", "[hle][trophy]")
{
    const std::vector<TrpFile> files = {
        {"TROPCONF.SFM", "<trophyconf/>"}, {"ICON0.PNG", "icon"}, {"TROP000.PNG", "t0"}};
    const std::vector<u8> v3 = make_trp(files, 3);
    std::string why;
    std::optional<trophy::TrpArchive> trp = trophy::TrpArchive::parse(v3, &why);
    REQUIRE(trp);
    CHECK(trp->version() == 3);
    REQUIRE(trp->entries().size() == 3);
    CHECK(trp->entries()[1].name == "ICON0.PNG");
    CHECK(trp->entries()[1].size == 4);
    CHECK(trp->entries()[1].offset % 16 == 0);
    CHECK(trp->required_space() == v3.size() - 0x40 - 3 * 0x40); // the files and their padding

    // The checksum (version 2 and later): one changed byte fails it; version 1 has none.
    std::vector<u8> broken = v3;
    broken.back() ^= 1;
    CHECK_FALSE(trophy::TrpArchive::parse(broken, &why));
    CHECK(why.find("SHA-1") != std::string::npos);
    std::vector<u8> v1 = make_trp(files, 1);
    std::memset(v1.data() + 0x1C, 0xEE, 20);
    CHECK(trophy::TrpArchive::parse(v1));
    // A header that claims more than the file holds isn't checked (RPCS3 can't read that much and goes on).
    std::vector<u8> longer = v3;
    ps_st64(longer.data() + 8, longer.size() + 0x100);
    CHECK(trophy::TrpArchive::parse(longer));
    // Not a pack: a bad magic, too short, entries past the end.
    std::vector<u8> magic = v3;
    magic[0] = 0;
    CHECK_FALSE(trophy::TrpArchive::parse(magic));
    CHECK_FALSE(trophy::TrpArchive::parse(std::vector<u8>(v3.begin(), v3.begin() + 0x3F)));
    std::vector<u8> count = make_trp(files, 1);
    ps_st32(count.data() + 16, 1000);
    CHECK_FALSE(trophy::TrpArchive::parse(count));

    // Entry names: present, renamed, removed (every entry with the name); names of 32 characters or more are ignored.
    CHECK(trp->contains("TROP000.PNG"));
    CHECK_FALSE(trp->contains("TROP001.PNG"));
    trp->rename("TROP000.PNG", "TROP009.PNG");
    CHECK(trp->contains("TROP009.PNG"));
    CHECK_FALSE(trp->contains("TROP000.PNG"));
    const std::string long_name(32, 'X');
    trp->rename("TROP009.PNG", long_name);
    CHECK(trp->contains("TROP009.PNG"));
    trp->remove("ICON0.PNG");
    CHECK(trp->entries().size() == 2);

    // Installing: the files replace the directory, whose TROPUSR.DAT stays.
    const TempDir dir("install");
    const fsys::path dest = dir.path / "trophy" / "NPWR12345_00";
    fsys::create_directories(dest);
    write_text(dest / "TROPUSR.DAT", "progress");
    write_text(dest / "OLD.PNG", "stale");
    REQUIRE(trp->install(dest, &why));
    CHECK(read_text(dest / "TROPCONF.SFM") == "<trophyconf/>");
    CHECK(read_text(dest / "TROP009.PNG") == "t0");
    CHECK(read_text(dest / "TROPUSR.DAT") == "progress");
    CHECK_FALSE(fsys::exists(dest / "OLD.PNG"));
    CHECK_FALSE(fsys::exists(dest / "ICON0.PNG"));
    usize entries = 0;
    for ([[maybe_unused]] const auto& e : fsys::directory_iterator(dir.path / "trophy")) entries++;
    CHECK(entries == 1); // no temporary directory left

    // An entry that would leave the directory fails the installation, which changes nothing; an entry outside the
    // file is skipped.
    std::optional<trophy::TrpArchive> evil = trophy::TrpArchive::parse(make_trp({{"../evil", "x"}}, 1));
    REQUIRE(evil);
    CHECK_FALSE(evil->install(dest, &why));
    CHECK(fsys::exists(dest / "TROPCONF.SFM"));
    CHECK_FALSE(fsys::exists(dir.path / "trophy" / "evil"));
    std::vector<u8> outside = make_trp({{"A.PNG", "a"}, {"B.PNG", "b"}}, 1);
    ps_st64(outside.data() + 0x40 + 32, 0x100000); // A.PNG's offset
    std::optional<trophy::TrpArchive> partial = trophy::TrpArchive::parse(outside);
    REQUIRE(partial);
    const fsys::path other = dir.path / "trophy" / "NPWR00001_00";
    REQUIRE(partial->install(other, &why));
    CHECK_FALSE(fsys::exists(other / "A.PNG"));
    CHECK(read_text(other / "B.PNG") == "b");
    // Two entries with one name: the second can't be created.
    std::optional<trophy::TrpArchive> twice = trophy::TrpArchive::parse(make_trp({{"A.PNG", "1"}, {"A.PNG", "2"}}, 1));
    REQUIRE(twice);
    CHECK_FALSE(twice->install(dir.path / "trophy" / "NPWR00002_00", &why));
    CHECK_FALSE(fsys::exists(dir.path / "trophy" / "NPWR00002_00"));
}

TEST_CASE("trophy files: TROPUSR.DAT generated from the configuration, saved, read back; platinum links",
    "[hle][trophy]")
{
    const TempDir dir("tropusr");
    const fsys::path conf = dir.path / "TROPCONF.SFM", usr = dir.path / "TROPUSR.DAT";
    write_text(conf, kTropEnglish);

    // Generated: every trophy of the configuration, locked, in RPCS3's layout.
    trophy::TropUsr a;
    const trophy::TropUsr::LoadResult generated = a.load(usr, conf);
    CHECK(generated.success);
    CHECK_FALSE(generated.discarded_existing);
    REQUIRE(a.trophy_count() == 4);
    CHECK(a.grade(0) == trophy::TropUsr::Platinum);
    CHECK(a.grade(1) == trophy::TropUsr::Bronze);
    CHECK(a.grade(2) == trophy::TropUsr::Gold);
    CHECK(a.grade(3) == trophy::TropUsr::Silver);
    CHECK(a.grade(4) == trophy::TropUsr::Unknown);
    CHECK_FALSE(a.unlocked(1));
    CHECK(a.unlocked_count() == 0);
    const std::vector<u8> bytes = read_bytes(usr);
    REQUIRE(bytes.size() == 0x30 + 2 * 0x20 + 4 * 0x60 + 4 * 0x70);
    const u8* d = bytes.data();
    CHECK(ps_ld32(d + 0x00) == 0x818F54AD);
    CHECK(ps_ld32(d + 0x04) == 0x00010000);
    CHECK(ps_ld32(d + 0x08) == 2);
    // Table headers: type, entry size, 1, count, offset.
    CHECK(ps_ld32(d + 0x30) == 4);
    CHECK(ps_ld32(d + 0x34) == 0x50);
    CHECK(ps_ld32(d + 0x38) == 1);
    CHECK(ps_ld32(d + 0x3C) == 4);
    CHECK(ps_ld64(d + 0x40) == 0x70);
    CHECK(ps_ld32(d + 0x50) == 6);
    CHECK(ps_ld32(d + 0x54) == 0x60);
    CHECK(ps_ld32(d + 0x5C) == 4);
    CHECK(ps_ld64(d + 0x60) == 0x70 + 4 * 0x60);
    // Table 4, trophy 2: type 4, size 0x50, index 2, 0, ID 2, gold, link 0. Table 6, trophy 3: type 6, size 0x60,
    // index 3, 0, ID 3, locked.
    const u8* e4 = d + 0x70 + 2 * 0x60;
    CHECK(ps_ld32(e4 + 0x00) == 4);
    CHECK(ps_ld32(e4 + 0x04) == 0x50);
    CHECK(ps_ld32(e4 + 0x08) == 2);
    CHECK(ps_ld32(e4 + 0x10) == 2);
    CHECK(ps_ld32(e4 + 0x14) == trophy::TropUsr::Gold);
    CHECK(ps_ld32(e4 + 0x18) == 0);
    CHECK(ps_ld32(d + 0x70 + 0x18) == 0xFFFFFFFFu); // the platinum trophy links to nothing
    const u8* e6 = d + 0x70 + 4 * 0x60 + 3 * 0x70;
    CHECK(ps_ld32(e6 + 0x00) == 6);
    CHECK(ps_ld32(e6 + 0x04) == 0x60);
    CHECK(ps_ld32(e6 + 0x08) == 3);
    CHECK(ps_ld32(e6 + 0x10) == 3);
    CHECK(ps_ld32(e6 + 0x14) == 0);

    // Unlocking: the platinum trophy completes when every trophy linked to it is unlocked (not the silver one).
    REQUIRE(a.unlock(1, 111, 222));
    CHECK(a.unlocked_platinum_id(1, conf) == trophy::TropUsr::kInvalidTrophyId);
    REQUIRE(a.unlock(3, 333, 444));
    CHECK(a.unlocked_platinum_id(3, conf) == trophy::TropUsr::kInvalidTrophyId);
    REQUIRE(a.unlock(2, 555, 666));
    CHECK(a.unlocked_platinum_id(2, conf) == 0);
    REQUIRE(a.unlock(0, 555, 666));
    CHECK(a.unlocked_platinum_id(2, conf) == trophy::TropUsr::kInvalidTrophyId); // already unlocked
    CHECK(a.unlocked_platinum_id(4, conf) == trophy::TropUsr::kInvalidTrophyId);
    CHECK_FALSE(a.unlock(4, 1, 1));
    CHECK(a.timestamp(1) == 222); // timestamp2
    CHECK(a.unlocked_count() == 4);
    REQUIRE(a.save(usr));

    // Read back as written.
    trophy::TropUsr b;
    const trophy::TropUsr::LoadResult loaded = b.load(usr, conf);
    CHECK(loaded.success);
    CHECK_FALSE(loaded.discarded_existing);
    CHECK(b.trophy_count() == 4);
    CHECK(b.unlocked(2));
    CHECK(b.timestamp(3) == 444);
    CHECK(read_bytes(usr).size() == bytes.size());
    const std::vector<u8> after = read_bytes(usr);
    CHECK(ps_ld64(after.data() + 0x70 + 4 * 0x60 + 1 * 0x70 + 0x20) == 111); // timestamp1 of trophy 1

    // An invalid file is generated again, every trophy locked; without a configuration nothing can be generated.
    for (const u32 size : {0u, 0x10u, 0x80u}) {
        std::vector<u8> cut(after.begin(), after.begin() + size);
        std::ofstream(usr, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(cut.data()), size);
        trophy::TropUsr c;
        const trophy::TropUsr::LoadResult again = c.load(usr, conf);
        CHECK(again.success);
        CHECK(again.discarded_existing);
        CHECK(c.trophy_count() == 4);
        CHECK(c.unlocked_count() == 0);
    }
    fsys::remove(usr);
    trophy::TropUsr none;
    CHECK_FALSE(none.load(usr, dir.path / "MISSING.SFM").success);
    CHECK_FALSE(fsys::exists(usr));
}
